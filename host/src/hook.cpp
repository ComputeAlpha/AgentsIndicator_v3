// `agent-indicator hook` - run by Claude Code for each lifecycle event.
// Contract: never blocks Claude, never prints, always exits 0.
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "commands.hpp"
#include "config.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "wire.hpp"

namespace {

std::string read_stdin_capped(size_t cap) {
    std::string in;
    char buf[65536];
    while (true) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n > 0) {
            if (in.size() < cap) in.append(buf, (size_t)n);
            continue;   // keep draining so Claude never blocks writing to us
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return in;
}

bool send_datagram(const std::string& msg) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = plat::socket_path();
    if (path.size() >= sizeof addr.sun_path) { close(fd); return false; }
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ssize_t n = sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT, (sockaddr*)&addr, sizeof addr);
    close(fd);
    return n == (ssize_t)msg.size();
}

void spawn_daemon() {
    std::string exe = plat::self_exe();
    if (exe.empty()) return;
    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) waitpid(pid, nullptr, 0);   // reap the intermediate child
        return;
    }
    // child: detach fully so Claude's hook runner doesn't wait on us
    setsid();
    if (fork() != 0) _exit(0);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }
    execl(exe.c_str(), exe.c_str(), "daemon", (char*)nullptr);
    _exit(0);
}

}  // namespace

int cmd_hook(int, char**) {
    signal(SIGPIPE, SIG_IGN);
    // 1 MB is plenty for the fields we use; big tool outputs are drained, not stored.
    const std::string input = read_stdin_capped(1 << 20);

    wire::Event ev;
    try {
        json::Value v = json::parse(input);
        if (!v.is_object()) return 0;
        ev.name = v.get_string("hook_event_name");
        ev.session = v.get_string("session_id");
        ev.agent_id = v.get_string("agent_id");
        ev.tool = v.get_string("tool_name");
        ev.cwd = v.get_string("cwd");
        if (ev.name == "SessionStart") ev.detail = v.get_string("source");
        else if (ev.name == "SessionEnd") ev.detail = v.get_string("reason");
        else if (ev.name == "Notification") ev.detail = v.get_string("notification_type");
        else if (ev.name == "StopFailure") ev.detail = v.get_string("error");
        else if (ev.name == "PreCompact" || ev.name == "PostCompact") ev.detail = v.get_string("trigger");
        else if (ev.name == "PostToolUseFailure") {
            const json::Value* intr = v.find("is_interrupt");
            if (intr && intr->type() == json::Value::Type::Bool && intr->as_bool()) ev.detail = "interrupt";
        }
    } catch (...) {
        return 0;   // not our business to complain inside Claude's UI
    }
    if (ev.name.empty() || ev.session.empty()) return 0;
    ev.pid = plat::find_session_process(getppid());

    const std::string msg = wire::encode(ev);
    if (send_datagram(msg)) return 0;

    // No daemon listening. Start one (it single-instances itself) and retry briefly.
    if (!Config::load().autostart) return 0;
    spawn_daemon();
    for (int i = 0; i < 15; ++i) {
        usleep(20'000);
        if (send_datagram(msg)) break;
    }
    return 0;
}
