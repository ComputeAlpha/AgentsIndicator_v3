#include "feed.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // macOS: SIGPIPE is ignored by the daemon instead
#endif

namespace feed {

namespace {

constexpr int64_t kRetryMs = 3000;          // nothing listening locally (no tunnel)
constexpr int64_t kMaxBackoffMs = 60'000;   // tunnel up but the far daemon isn't
constexpr int64_t kStaleMs = 5000;          // feed silent this long -> sessions dropped
constexpr int64_t kDeadMs = 15'000;         // ... and the connection recycled
constexpr size_t kMaxLine = 256 * 1024;
constexpr size_t kMaxClients = 32;

char state_char(SessState s) {
    switch (s) {
    case SessState::Working: return 'W';
    case SessState::Waiting: return 'Y';
    case SessState::Idle: return 'I';
    case SessState::Done: return 'D';
    case SessState::Error: return 'E';
    }
    return 'I';
}

bool char_state(char c, SessState& out) {
    switch (c) {
    case 'W': out = SessState::Working; return true;
    case 'Y': out = SessState::Waiting; return true;
    case 'I': out = SessState::Idle; return true;
    case 'D': out = SessState::Done; return true;
    case 'E': out = SessState::Error; return true;
    }
    return false;
}

json::Value num(long long n) { return json::Value::number(n); }

bool to_ll(const json::Value& v, long long& out) {
    if (v.type() != json::Value::Type::Number) return false;
    char* end = nullptr;
    out = strtoll(v.str().c_str(), &end, 10);
    return end && *end == 0;
}

void set_flags(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

}  // namespace

// ------------------------------------------------------------ wire format

std::string encode(const View& v, const std::string& host) {
    json::Value root = json::Value::object();
    root["v"] = num(kVersion);
    root["host"] = json::Value::string(host);
    json::Value ss = json::Value::array();
    for (const ViewSession& s : v.sessions) {
        json::Value a = json::Value::array();
        a.push(json::Value::string(std::string(1, state_char(s.state))));
        a.push(json::Value::string(s.state != SessState::Waiting ? ""
                                   : s.wait == WaitKind::Permission ? "p" : "t"));
        a.push(num(s.in_state_ms));
        a.push(num(s.streak_ms));
        ss.push(std::move(a));
    }
    root["s"] = std::move(ss);
    const DayMetrics& m = v.day;
    json::Value d = json::Value::array();
    for (long long x : {m.agent_work_ms, m.busy_wall_ms, m.wait_total_ms, m.wait_count, m.idle_total_ms,
                        m.idle_count, m.tool_calls, m.sessions_started})
        d.push(num(x));
    root["d"] = std::move(d);
    root["apm"] = num((long long)(v.actions_per_min * 100.0 + 0.5));
    root["ia"] = num(v.inactive_ms);
    return root.dump(0);
}

bool decode(const std::string& line, View& out, std::string& host) {
    json::Value root;
    try {
        root = json::parse(line);
    } catch (...) {
        return false;
    }
    if (!root.is_object()) return false;
    long long ver = 0;
    const json::Value* pv = root.find("v");
    if (!pv || !to_ll(*pv, ver) || ver != kVersion) return false;

    View v;
    const json::Value* ss = root.find("s");
    if (!ss || !ss->is_array()) return false;
    for (const json::Value& a : ss->items()) {
        if (!a.is_array() || a.items().size() < 4) return false;
        const auto& it = a.items();
        ViewSession s;
        long long st = 0, wk = 0;
        if (!it[0].is_string() || it[0].str().size() != 1 || !char_state(it[0].str()[0], s.state)) return false;
        if (!it[1].is_string() || !to_ll(it[2], st) || !to_ll(it[3], wk)) return false;
        if (s.state == SessState::Waiting) s.wait = it[1].str() == "p" ? WaitKind::Permission : WaitKind::TurnEnd;
        s.in_state_ms = std::max(0LL, st);
        s.streak_ms = std::max(0LL, wk);
        v.sessions.push_back(s);
    }
    const json::Value* d = root.find("d");
    if (!d || !d->is_array() || d->items().size() < 8) return false;
    long long x[8];
    for (int i = 0; i < 8; ++i)
        if (!to_ll(d->items()[(size_t)i], x[i]) || x[i] < 0) return false;
    v.day.agent_work_ms = x[0];
    v.day.busy_wall_ms = x[1];
    v.day.wait_total_ms = x[2];
    v.day.wait_count = x[3];
    v.day.idle_total_ms = x[4];
    v.day.idle_count = x[5];
    v.day.tool_calls = x[6];
    v.day.sessions_started = x[7];
    long long apm = 0, ia = 0;
    if (const json::Value* p = root.find("apm"); p && to_ll(*p, apm)) v.actions_per_min = std::max(0LL, apm) / 100.0;
    if (const json::Value* p = root.find("ia"); p && to_ll(*p, ia)) v.inactive_ms = std::max(0LL, ia);
    host = root.get_string("host");
    out = std::move(v);
    return true;
}

// ------------------------------------------------------------ server

bool Server::listen(int port) {
    close_all();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    set_flags(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never exposed to the network
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 8) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return false;
    }
    fd_ = fd;
    return true;
}

void Server::accept_pending() {
    if (fd_ < 0) return;
    while (true) {
        int c = accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        if (clients_.size() >= kMaxClients) { close(c); continue; }
        set_flags(c);
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        clients_.push_back(c);
    }
}

void Server::publish(const std::string& line) {
    const std::string msg = line + "\n";
    for (size_t i = 0; i < clients_.size();) {
        ssize_t n = send(clients_[i], msg.data(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n == (ssize_t)msg.size()) { ++i; continue; }
        // Gone, or too slow to take one small line: drop; it will reconnect.
        close(clients_[i]);
        clients_.erase(clients_.begin() + (long)i);
    }
}

void Server::close_all() {
    for (int c : clients_) close(c);
    clients_.clear();
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

// ------------------------------------------------------------ remote

Remote::Remote(std::string target) : target_(std::move(target)) {
    std::string p = target_;
    if (size_t colon = target_.rfind(':'); colon != std::string::npos) {
        addr_ = target_.substr(0, colon);
        p = target_.substr(colon + 1);
        if (addr_ == "localhost" || addr_.empty()) addr_ = "127.0.0.1";
    }
    port_ = atoi(p.c_str());
    host_ = target_;
}

void Remote::disconnect() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    connecting_ = false;
    buf_.clear();
}

void Remote::fail(int64_t now, const char* why) {
    const bool was_receiving = announced_;
    disconnect();
    if (was_receiving) {
        log_.push_back("feed " + host_ + " (" + target_ + ") lost: " + why);
        backoff_ms_ = kRetryMs;
    }
    announced_ = false;
    next_try_ = now + backoff_ms_;
}

bool Remote::poll(int64_t now) {
    const bool was_live = live(now);
    bool changed = false;

    if (fd_ < 0 && now >= next_try_ && port_ > 0 && port_ < 65536) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            set_flags(fd);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port_);
            if (inet_pton(AF_INET, addr_.c_str(), &a.sin_addr) != 1) {
                close(fd);
                next_try_ = now + kMaxBackoffMs;
                return false;
            }
            const int r = connect(fd, (sockaddr*)&a, sizeof a);
            if (r == 0 || errno == EINPROGRESS) {
                fd_ = fd;
                connecting_ = r != 0;
                next_try_ = now;   // reference for the "no data yet" timeout
            } else {
                close(fd);
                next_try_ = now + kRetryMs;   // no tunnel right now
            }
        }
    }

    if (fd_ >= 0 && connecting_) {
        pollfd p{fd_, POLLOUT, 0};
        if (::poll(&p, 1, 0) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) {
                disconnect();
                next_try_ = now + kRetryMs;
            } else {
                connecting_ = false;
            }
        }
    }

    if (fd_ >= 0 && !connecting_) {
        char tmp[4096];
        while (true) {
            ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
            if (n > 0) {
                buf_.append(tmp, (size_t)n);
                if (buf_.size() > kMaxLine) { fail(now, "oversized line"); break; }
                continue;
            }
            if (n == 0) {
                // SSH accepted the forward but the far end refused or went away.
                // Back off so ssh doesn't print "open failed" every few seconds.
                if (!announced_) backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
                fail(now, "closed");
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fail(now, strerror(errno));
            }
            break;
        }
        size_t nl;
        while (fd_ >= 0 && (nl = buf_.find('\n')) != std::string::npos) {
            std::string line = buf_.substr(0, nl);
            buf_.erase(0, nl + 1);
            View v;
            std::string host;
            if (!decode(line, v, host)) { fail(now, "not an agent-indicator feed"); break; }
            last_ = std::move(v);
            last_rx_ = now;
            changed = true;
            backoff_ms_ = kRetryMs;
            if (!host.empty()) host_ = host;
            if (!announced_) {
                announced_ = true;
                log_.push_back("feed " + host_ + " (" + target_ + ") connected");
            }
        }
        // Connected but silent (stalled network, wrong service): recycle.
        if (fd_ >= 0) {
            const int64_t since = last_rx_ >= 0 && announced_ ? last_rx_ : next_try_;
            if (now - since > kDeadMs) {
                if (!announced_) backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
                fail(now, "timed out");
            }
        }
    }

    return changed || was_live != live(now);
}

bool Remote::live(int64_t now) const {
    return fd_ >= 0 && announced_ && last_rx_ >= 0 && now - last_rx_ <= kStaleMs;
}

View Remote::view(int64_t now) const {
    if (!live(now)) return {};
    View v = last_;
    const int64_t age = now - last_rx_;
    for (ViewSession& s : v.sessions) {
        s.in_state_ms += age;
        s.streak_ms += age;
    }
    v.inactive_ms += age;
    return v;
}

}  // namespace feed
