#include "serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "platform.hpp"

namespace {

std::vector<std::string> glob_paths(const char* pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (glob(pattern, 0, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g);
    return out;
}

#ifdef __linux__
std::string sysfs_attr(const std::string& tty, const char* attr) {
    // /sys/class/tty/ttyACM0/device -> interface dir; the USB device is its parent
    std::string v;
    plat::read_file("/sys/class/tty/" + tty + "/device/../" + attr, v);
    while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
    return v;
}
#endif

}  // namespace

std::vector<std::string> SerialLink::candidates() {
    std::vector<std::string> out;
#ifdef __linux__
    // Our own USB product string first, then any Raspberry Pi VID CDC port.
    for (auto& p : glob_paths("/dev/serial/by-id/*AgentIndicator_v3*")) out.push_back(p);
    for (auto& p : glob_paths("/dev/ttyACM*")) {
        std::string name = p.substr(5);
        if (sysfs_attr(name, "idVendor") == "2e8a") out.push_back(p);
    }
#else
    for (auto& p : glob_paths("/dev/cu.usbmodem*")) out.push_back(p);
#endif
    // de-duplicate by resolved target
    std::vector<std::string> uniq, seen;
    for (auto& p : out) {
        char buf[4096];
        std::string real = realpath(p.c_str(), buf) ? buf : p;
        if (std::find(seen.begin(), seen.end(), real) == seen.end()) {
            seen.push_back(real);
            uniq.push_back(p);
        }
    }
    return uniq;
}

bool SerialLink::open(const std::string& path_or_empty) {
    close();
    if (!path_or_empty.empty()) return open_one(path_or_empty);
    for (const auto& c : candidates())
        if (open_one(c)) return true;
    return false;
}

bool SerialLink::open_one(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (ioctl(fd, TIOCEXCL) != 0 && errno != ENOTTY) { /* best effort */ }

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) { ::close(fd); return false; }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(tcflag_t)HUPCL;
    cfsetispeed(&tio, B115200);   // ignored by USB CDC, but keeps drivers happy
    cfsetospeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);
    int dtr = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &dtr);    // the Pico SDK only sends to a "connected" host
    tcflush(fd, TCIOFLUSH);

    fd_ = fd;
    path_ = path;
    rx_.clear();
    // Handshake: up to 3 tries over ~1.5 s (the device may still be enumerating).
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!send_line("?")) break;
        const int64_t deadline = plat::mono_ms() + 500;
        while (plat::mono_ms() < deadline) {
            pollfd p{fd_, POLLIN, 0};
            poll(&p, 1, 50);
            for (auto& l : read_lines()) {
                if (l.rfind("AGENTIND v3", 0) == 0) {
                    ident_ = l;
                    return true;
                }
            }
        }
    }
    close();
    return false;
}

void SerialLink::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    path_.clear();
    ident_.clear();
}

bool SerialLink::send_line(const std::string& line) {
    if (fd_ < 0) return false;
    std::string data = line + "\n";
    size_t off = 0;
    const int64_t deadline = plat::mono_ms() + 200;
    while (off < data.size()) {
        ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (plat::mono_ms() > deadline) return true;   // device busy: drop this frame, keep link
            pollfd p{fd_, POLLOUT, 0};
            poll(&p, 1, 20);
            continue;
        }
        close();
        return false;
    }
    return true;
}

std::vector<std::string> SerialLink::read_lines() {
    std::vector<std::string> lines;
    if (fd_ < 0) return lines;
    char buf[512];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n > 0) { rx_.append(buf, (size_t)n); continue; }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(); break; }
        break;
    }
    size_t pos;
    while ((pos = rx_.find('\n')) != std::string::npos) {
        std::string l = rx_.substr(0, pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        rx_.erase(0, pos + 1);
        if (!l.empty()) lines.push_back(l);
    }
    if (rx_.size() > 4096) rx_.clear();
    return lines;
}
