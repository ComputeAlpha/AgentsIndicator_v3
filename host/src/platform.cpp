#include "platform.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc_info.h>
#endif

namespace plat {

int64_t mono_ms() {
    timespec ts{};
#ifdef __linux__
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t wall_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int local_day() {
    time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

std::string home_dir() {
    if (const char* h = getenv("HOME"); h && *h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

std::string config_dir() {
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/agent-indicator";
    return home_dir() + "/.config/agent-indicator";
}

std::string state_dir() {
    if (const char* x = getenv("XDG_STATE_HOME"); x && *x) return std::string(x) + "/agent-indicator";
    return home_dir() + "/.local/state/agent-indicator";
}

std::string socket_path() {
    // sun_path is 104 (macOS) / 108 (Linux) bytes; fall back for very long homes.
    std::string p = state_dir() + "/daemon.sock";
    if (p.size() < 100) return p;
    return "/tmp/agent-indicator-" + std::to_string(getuid()) + ".sock";
}

std::string self_exe() {
    char buf[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[PATH_MAX];
        if (realpath(buf, real)) return real;
        return buf;
    }
    return {};
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return buf;
#endif
}

bool mkdirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) { close(fd); unlink(tmp.c_str()); return false; }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    // keep the target's permissions if it exists
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
    if (rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

bool proc_info(pid_t pid, ProcInfo& out) {
    if (pid <= 0) return false;
#ifdef __APPLE__
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != (int)sizeof info) return false;
    out.ppid = (pid_t)info.pbi_ppid;
    out.name = info.pbi_name[0] ? info.pbi_name : info.pbi_comm;
    out.start_id = (uint64_t)info.pbi_start_tvsec * 1000000ull + info.pbi_start_tvusec;
    return true;
#else
    std::string stat;
    if (!read_file("/proc/" + std::to_string(pid) + "/stat", stat)) return false;
    // "pid (comm) state ppid ... starttime(22)"; comm may contain spaces/parens.
    size_t open = stat.find('('), close = stat.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    out.name = stat.substr(open + 1, close - open - 1);
    std::istringstream rest(stat.substr(close + 2));
    std::string field;
    std::vector<std::string> f;
    while (rest >> field) f.push_back(field);
    // f[0] = state (field 3), f[1] = ppid (4), starttime is field 22 -> f[19]
    if (f.size() < 20) return false;
    out.ppid = (pid_t)atoi(f[1].c_str());
    out.start_id = strtoull(f[19].c_str(), nullptr, 10);
    return true;
#endif
}

bool proc_alive(pid_t pid, uint64_t start_id) {
    if (pid <= 0) return true;   // unknown PID: can't tell, assume alive
    if (kill(pid, 0) != 0 && errno == ESRCH) return false;
    ProcInfo pi;
    if (!proc_info(pid, pi)) return errno == EPERM;   // exists but hidden
    return start_id == 0 || pi.start_id == start_id;
}

static bool is_shell(const std::string& n) {
    static const char* shells[] = {"sh", "bash", "zsh", "dash", "fish", "ksh", "-sh", "-bash", "-zsh", "timeout", "env"};
    for (const char* s : shells)
        if (n == s) return true;
    return false;
}

pid_t find_session_process(pid_t start) {
    pid_t pid = start;
    for (int depth = 0; depth < 4 && pid > 1; ++depth) {
        ProcInfo pi;
        if (!proc_info(pid, pi)) return start;
        if (!is_shell(pi.name)) return pid;
        pid = pi.ppid;
    }
    return start;
}

}  // namespace plat
