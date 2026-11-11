// `agent-indicator daemon` - owns the session model, the clock, and the device.
#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "commands.hpp"
#include "composer.hpp"
#include "config.hpp"
#include "feed.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "serial.hpp"
#include "tracker.hpp"
#include "wire.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_reload = 0;
bool g_verbose = false;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    char ts[32];
    time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &lt);
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

int bind_socket(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str());   // we hold the instance lock, so any old socket is stale
    if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0) { close(fd); return -1; }
    chmod(path.c_str(), 0600);
    // A burst of hooks (parallel tool calls) must not be dropped.
    int rcv = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    return fd;
}

std::string short_hostname() {
    char h[256] = {};
    if (gethostname(h, sizeof h - 1) != 0) return "unknown";
    std::string s = h;
    if (size_t dot = s.find('.'); dot != std::string::npos) s.resize(dot);
    return s;
}

using Remotes = std::vector<std::unique_ptr<feed::Remote>>;

// Feed endpoints from the config. A remote pointing at our own broadcast port
// would show every session twice, so it is skipped.
Remotes make_remotes(const Config& cfg) {
    Remotes out;
    for (const auto& r : cfg.remotes) {
        auto rem = std::make_unique<feed::Remote>(r);
        const std::string port = r.substr(r.rfind(':') == std::string::npos ? 0 : r.rfind(':') + 1);
        if (cfg.broadcast_port > 0 && atoi(port.c_str()) == cfg.broadcast_port) {
            logf("remotes: skipping %s (that's this daemon's own broadcast port)", r.c_str());
            continue;
        }
        out.push_back(std::move(rem));
    }
    return out;
}

void start_broadcast(feed::Server& server, const Config& cfg) {
    server.close_all();
    if (cfg.broadcast_port <= 0) return;
    if (server.listen(cfg.broadcast_port))
        logf("broadcasting sessions on 127.0.0.1:%d", cfg.broadcast_port);
    else
        logf("cannot broadcast on 127.0.0.1:%d: %s", cfg.broadcast_port, strerror(errno));
}

std::string status_json(const Tracker& t, const SerialLink& link, const PanelText panels[4], int64_t now,
                        bool starry, const Remotes& remotes, const feed::Server& server) {
    json::Value root = json::Value::object();
    root["updated"] = json::Value::number((long long)plat::wall_ms());
    root["device"] = json::Value::string(link.is_open() ? link.path() : "");
    root["device_ident"] = json::Value::string(link.ident());
    root["screensaver"] = json::Value::boolean(starry);
    json::Value ps = json::Value::array();
    for (int i = 0; i < 4; ++i) ps.push(json::Value::string(panels[i].label));
    root["panels"] = std::move(ps);

    DayMetrics m = t.live_metrics(now);
    json::Value mj = json::Value::object();
    mj["agent_work_ms"] = json::Value::number((long long)m.agent_work_ms);
    mj["busy_wall_ms"] = json::Value::number((long long)m.busy_wall_ms);
    mj["avg_wait_ms"] = json::Value::number((long long)(m.wait_count ? m.wait_total_ms / m.wait_count : 0));
    mj["waits"] = json::Value::number((long long)m.wait_count);
    mj["avg_idle_ms"] = json::Value::number((long long)(m.idle_count ? m.idle_total_ms / m.idle_count : 0));
    mj["idles"] = json::Value::number((long long)m.idle_count);
    mj["tool_calls"] = json::Value::number((long long)m.tool_calls);
    mj["sessions_started"] = json::Value::number((long long)m.sessions_started);
    mj["actions_per_min"] = json::Value::number((long long)(t.actions_per_min(now) + 0.5));
    root["today"] = std::move(mj);

    json::Value arr = json::Value::array();
    for (const Session* s : t.ordered()) {
        json::Value o = json::Value::object();
        o["id"] = json::Value::string(s->id);
        o["state"] = json::Value::string(state_name(s->state));
        if (s->state == SessState::Waiting)
            o["wait"] = json::Value::string(s->wait == WaitKind::Permission ? "permission" : "turn_end");
        if (!s->error.empty()) o["error"] = json::Value::string(s->error);
        o["in_state_ms"] = json::Value::number((long long)(now - s->state_since));
        o["age_ms"] = json::Value::number((long long)(now - s->created));
        o["pid"] = json::Value::number((long long)s->pid);
        o["cwd"] = json::Value::string(s->cwd);
        arr.push(std::move(o));
    }
    root["sessions"] = std::move(arr);

    root["broadcast_clients"] = json::Value::number((long long)server.clients());
    json::Value rs = json::Value::array();
    for (const auto& r : remotes) {
        json::Value o = json::Value::object();
        o["target"] = json::Value::string(r->target());
        o["host"] = json::Value::string(r->host());
        o["live"] = json::Value::boolean(r->live(now));
        json::Value ss = json::Value::array();
        for (const ViewSession& s : r->view(now).sessions) {
            json::Value so = json::Value::object();
            so["state"] = json::Value::string(state_name(s.state));
            if (s.state == SessState::Waiting)
                so["wait"] = json::Value::string(s.wait == WaitKind::Permission ? "permission" : "turn_end");
            so["in_state_ms"] = json::Value::number((long long)s.in_state_ms);
            ss.push(std::move(so));
        }
        o["sessions"] = std::move(ss);
        rs.push(std::move(o));
    }
    root["remotes"] = std::move(rs);
    return root.dump(2) + "\n";
}

}  // namespace

int cmd_daemon(int argc, char** argv) {
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) g_verbose = true;

    const std::string sdir = plat::state_dir();
    if (!plat::mkdirs(sdir)) { logf("cannot create %s: %s", sdir.c_str(), strerror(errno)); return 1; }

    // Single instance: the hook may race to autostart several daemons.
    int lock = open((sdir + "/daemon.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (g_verbose) logf("another daemon is running; exiting");
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = [](int) { g_stop = 1; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sa.sa_handler = [](int) { g_reload = 1; };
    sigaction(SIGHUP, &sa, nullptr);

    std::string warnings;
    Config cfg = Config::load(&warnings);
    if (!warnings.empty()) logf("%s", warnings.c_str());

    const int sock = bind_socket(plat::socket_path());
    if (sock < 0) { logf("cannot bind %s: %s", plat::socket_path().c_str(), strerror(errno)); return 1; }

    Tracker tracker(cfg.tracker);
    Composer composer(cfg);
    const std::string state_file = sdir + "/state.json";
    {
        std::string text;
        if (plat::read_file(state_file, text)) {
            try {
                tracker.load(json::parse(text), plat::mono_ms(), plat::wall_ms(), plat::local_day(), plat::proc_alive);
            } catch (const std::exception& e) {
                logf("ignoring unreadable state: %s", e.what());
            }
        }
        tracker.roll_day(plat::local_day(), plat::mono_ms());
        tracker.mark_activity(plat::mono_ms());   // screensaver counts from startup
    }
    logf("agent-indicator daemon started (socket %s)", plat::socket_path().c_str());

    const std::string host = short_hostname();
    feed::Server server;
    start_broadcast(server, cfg);
    Remotes remotes = make_remotes(cfg);

    SerialLink link;
    uint32_t seq = 0;
    std::string last_body;
    int64_t last_send = 0, last_tick = 0, last_probe = -1'000'000, last_save = plat::mono_ms(),
            last_status = 0;
    PanelText panels[4];
    std::string last_status_body;

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            warnings.clear();
            cfg = Config::load(&warnings);
            if (!warnings.empty()) logf("%s", warnings.c_str());
            logf("config reloaded (tracker timeouts apply after restart)");
            start_broadcast(server, cfg);
            remotes = make_remotes(cfg);
            if (link.is_open()) {
                link.send_line("B " + std::to_string(cfg.brightness));
                link.send_line("L " + std::to_string(cfg.current_budget_ma));
            }
        }

        // --- device (re)connect
        int64_t now = plat::mono_ms();
        if (!link.is_open() && now - last_probe >= 3000) {
            last_probe = now;
            if (link.open(cfg.serial_port)) {
                logf("device connected: %s (%s)", link.path().c_str(), link.ident().c_str());
                link.send_line("B " + std::to_string(cfg.brightness));
                link.send_line("L " + std::to_string(cfg.current_budget_ma));
                last_body.clear();   // force a full frame
            }
        }

        // --- wait for hooks / device input
        // fds[0] hooks, fds[1] device (or -1: ignored), then broadcast + feeds
        std::vector<pollfd> fds = {{sock, POLLIN, 0}, {link.is_open() ? link.fd() : -1, POLLIN, 0},
                                   {server.fd(), POLLIN, 0}};
        for (const auto& r : remotes) fds.push_back({r->fd(), POLLIN, 0});
        poll(fds.data(), (nfds_t)fds.size(), 250);
        now = plat::mono_ms();

        bool changed = false;
        if (fds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t n;
            while ((n = recv(sock, buf, sizeof buf, 0)) > 0) {
                std::string msg(buf, (size_t)n);
                wire::Event ev;
                if (wire::decode(msg, ev)) {
                    plat::ProcInfo pi;
                    uint64_t start = ev.pid > 0 && plat::proc_info(ev.pid, pi) ? pi.start_id : 0;
                    HookEvent he{ev.name, ev.session, ev.agent_id, ev.detail, ev.tool, ev.cwd, ev.pid};
                    tracker.on_event(he, now, start);
                    changed = true;
                    if (g_verbose)
                        logf("event %s session=%.8s agent=%s detail=%s tool=%s pid=%d", ev.name.c_str(),
                             ev.session.c_str(), ev.agent_id.c_str(), ev.detail.c_str(), ev.tool.c_str(),
                             (int)ev.pid);
                } else if (msg.rfind("CTL1\traw\t", 0) == 0) {
                    std::string line = msg.substr(9);
                    logf("ctl raw: %s", line.c_str());
                    if (link.is_open()) link.send_line(line);
                }
            }
        }
        if (link.is_open() && fds[1].fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (const auto& l : link.read_lines()) logf("device: %s", l.c_str());
            if (!link.is_open()) logf("device disconnected");
        }

        // --- feeds: new listeners, data from other machines
        if (fds[2].revents & POLLIN) server.accept_pending();
        for (auto& r : remotes) {
            if (r->poll(now)) changed = true;
            for (const auto& l : r->take_log()) logf("%s", l.c_str());
        }

        // --- housekeeping once per second
        if (now - last_tick >= 1000 || changed) {
            if (now - last_tick >= 1000) {
                tracker.roll_day(plat::local_day(), now);
                tracker.tick(now, plat::proc_alive);
                last_tick = now;
            }
            View view = make_view(tracker, now);
            if (server.clients()) server.publish(feed::encode(view, host));   // our sessions only
            for (const auto& r : remotes)
                if (r->live(now)) merge_view(view, r->view(now));
            const std::string body = composer.compose(view, now, panels);
            if (link.is_open() && (body != last_body || now - last_send >= 1000)) {
                if (link.send_line("F " + std::to_string(++seq) + " " + body)) {
                    if (g_verbose && body != last_body) logf("frame %s", body.c_str());
                    last_body = body;
                    last_send = now;
                } else {
                    logf("device write failed; reconnecting");
                }
            }
            if (body != last_status_body || now - last_status >= 5000) {
                last_status_body = body;
                plat::write_file_atomic(sdir + "/status.json",
                                        status_json(tracker, link, panels, now, composer.screensaver(view),
                                                    remotes, server));
                last_status = now;
            }
        }

        if (now - last_save >= 30'000) {
            plat::write_file_atomic(state_file, tracker.save(now, plat::wall_ms()).dump(2) + "\n");
            last_save = now;
        }
    }

    const int64_t now = plat::mono_ms();
    plat::write_file_atomic(state_file, tracker.save(now, plat::wall_ms()).dump(2) + "\n");
    unlink(plat::socket_path().c_str());
    logf("daemon stopped");
    return 0;
}
