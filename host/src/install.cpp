// install / uninstall / status / raw
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <algorithm>
#include <unistd.h>

#include "commands.hpp"
#include "composer.hpp"
#include "config.hpp"
#include "json.hpp"
#include "platform.hpp"

namespace {

// Every event the tracker understands. Matchers are omitted (= all).
const char* const kHookEvents[] = {
    "SessionStart", "SessionEnd",  "UserPromptSubmit", "PreToolUse",   "PostToolUse",
    "PostToolUseFailure", "PermissionRequest", "PermissionDenied", "Notification", "Stop",
    "StopFailure",  "SubagentStart", "SubagentStop",   "PreCompact",   "PostCompact",
    "Elicitation",  "ElicitationResult",
};

std::string settings_path() {
    if (const char* d = getenv("CLAUDE_CONFIG_DIR"); d && *d) return std::string(d) + "/settings.json";
    return plat::home_dir() + "/.claude/settings.json";
}

std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

bool is_our_hook(const json::Value& h) {
    if (!h.is_object()) return false;
    const std::string cmd = h.get_string("command");
    return cmd.find("agent-indicator") != std::string::npos && cmd.find(" hook") != std::string::npos;
}

// Removes our handlers from every event; drops groups/events left empty.
int strip_our_hooks(json::Value& settings) {
    json::Value* hooks = settings.find("hooks");
    if (!hooks || !hooks->is_object()) return 0;
    int removed = 0;
    auto& events = hooks->members();
    for (auto ev = events.begin(); ev != events.end();) {
        if (ev->second.is_array()) {
            auto& groups = ev->second.items();
            for (auto g = groups.begin(); g != groups.end();) {
                json::Value* hs = g->find("hooks");
                if (hs && hs->is_array()) {
                    auto& list = hs->items();
                    for (auto h = list.begin(); h != list.end();) {
                        if (is_our_hook(*h)) { h = list.erase(h); ++removed; }
                        else ++h;
                    }
                    if (list.empty()) { g = groups.erase(g); continue; }
                }
                ++g;
            }
            if (groups.empty()) { ev = events.erase(ev); continue; }
        }
        ++ev;
    }
    if (events.empty()) settings.erase("hooks");
    return removed;
}

bool load_settings(const std::string& path, json::Value& out, bool& existed) {
    std::string text;
    existed = plat::read_file(path, text);
    if (!existed || text.find_first_not_of(" \t\r\n") == std::string::npos) {
        out = json::Value::object();
        return true;
    }
    try {
        out = json::parse(text);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s is not valid JSON (%s); not touching it.\n", path.c_str(), e.what());
        return false;
    }
    if (!out.is_object()) {
        fprintf(stderr, "error: %s is not a JSON object; not touching it.\n", path.c_str());
        return false;
    }
    return true;
}

bool save_settings(const std::string& path, const json::Value& v, bool existed) {
    if (existed) {
        char ts[32];
        time_t now = time(nullptr);
        tm lt{};
        localtime_r(&now, &lt);
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
        std::string orig;
        plat::read_file(path, orig);
        std::string backup = path + ".bak-agent-indicator-" + ts;
        if (!plat::write_file_atomic(backup, orig)) {
            fprintf(stderr, "error: cannot write backup %s\n", backup.c_str());
            return false;
        }
        printf("  backup:   %s\n", backup.c_str());
    }
    std::string dir = path.substr(0, path.rfind('/'));
    plat::mkdirs(dir);
    if (!plat::write_file_atomic(path, v.dump(2) + "\n")) {
        fprintf(stderr, "error: cannot write %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

int run(const std::string& cmd) {
    int rc = system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

#ifdef __APPLE__
const char* kLaunchLabel = "com.agentindicator.daemon";
std::string plist_path() { return plat::home_dir() + "/Library/LaunchAgents/" + kLaunchLabel + ".plist"; }
#else
bool has_systemd_user() { return run("systemctl --user show-environment >/dev/null 2>&1") == 0; }
#endif

bool install_service(const std::string& exe) {
#ifdef __APPLE__
    const std::string log = plat::state_dir() + "/daemon.log";
    const std::string plist = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>)") + kLaunchLabel + R"(</string>
  <key>ProgramArguments</key>
  <array><string>)" + exe + R"(</string><string>daemon</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>StandardErrorPath</key><string>)" + log + R"(</string>
  <key>StandardOutPath</key><string>)" + log + R"(</string>
</dict>
</plist>
)";
    plat::mkdirs(plat::home_dir() + "/Library/LaunchAgents");
    if (!plat::write_file_atomic(plist_path(), plist)) return false;
    const std::string dom = "gui/" + std::to_string(getuid());
    run("launchctl bootout " + dom + " " + shell_quote(plist_path()) + " >/dev/null 2>&1");
    if (run("launchctl bootstrap " + dom + " " + shell_quote(plist_path())) != 0) return false;
    printf("  service:  launchd agent %s (log: %s)\n", kLaunchLabel, log.c_str());
    return true;
#else
    if (!has_systemd_user()) {
        printf("  service:  no systemd user session; the hook will autostart the daemon on demand\n");
        return true;
    }
    const std::string unit = "[Unit]\nDescription=AgentIndicator v3 daemon (Claude Code session LEDs)\n\n"
                             "[Service]\nExecStart=" + exe + " daemon\nRestart=always\nRestartSec=2\n\n"
                             "[Install]\nWantedBy=default.target\n";
    std::string dir = plat::home_dir() + "/.config/systemd/user";
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) dir = std::string(x) + "/systemd/user";
    plat::mkdirs(dir);
    if (!plat::write_file_atomic(dir + "/agent-indicator.service", unit)) return false;
    run("systemctl --user daemon-reload");
    if (run("systemctl --user enable agent-indicator.service >/dev/null 2>&1") != 0) return false;
    run("systemctl --user restart agent-indicator.service");
    printf("  service:  systemd user unit agent-indicator.service (logs: journalctl --user -u agent-indicator)\n");
    return true;
#endif
}

void remove_service() {
#ifdef __APPLE__
    run("launchctl bootout gui/" + std::to_string(getuid()) + " " + shell_quote(plist_path()) + " >/dev/null 2>&1");
    unlink(plist_path().c_str());
#else
    if (has_systemd_user()) run("systemctl --user disable --now agent-indicator.service >/dev/null 2>&1");
    std::string dir = plat::home_dir() + "/.config/systemd/user";
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) dir = std::string(x) + "/systemd/user";
    unlink((dir + "/agent-indicator.service").c_str());
    if (has_systemd_user()) run("systemctl --user daemon-reload");
#endif
}

bool has_flag(int argc, char** argv, const char* f) {
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], f)) return true;
    return false;
}

std::string fmt_ms(long long ms) {
    int v;
    char u;
    format_duration(ms, v, u);
    return std::to_string(v) + (u == 's' ? "s" : u == 'm' ? "m" : "h");
}

}  // namespace

int cmd_install(int argc, char** argv) {
    const std::string exe = plat::self_exe();
    if (exe.empty()) { fprintf(stderr, "error: cannot resolve own path\n"); return 1; }
    printf("Installing AgentIndicator v3 hooks for %s\n", exe.c_str());

    // config file (never overwritten)
    plat::mkdirs(plat::config_dir());
    const std::string cfg = plat::config_dir() + "/config";
    struct stat st{};
    if (stat(cfg.c_str(), &st) != 0) {
        plat::write_file_atomic(cfg, Config::default_text());
        printf("  config:   %s (new)\n", cfg.c_str());
    } else {
        printf("  config:   %s (kept)\n", cfg.c_str());
    }
    plat::mkdirs(plat::state_dir());

    // hooks
    const std::string path = settings_path();
    json::Value settings;
    bool existed = false;
    if (!load_settings(path, settings, existed)) return 1;
    strip_our_hooks(settings);
    json::Value& hooks = settings["hooks"];
    if (!hooks.is_object()) hooks = json::Value::object();
    const std::string command = shell_quote(exe) + " hook";
    for (const char* ev : kHookEvents) {
        json::Value handler = json::Value::object();
        handler["type"] = json::Value::string("command");
        handler["command"] = json::Value::string(command);
        handler["timeout"] = json::Value::number(5LL);
        json::Value group = json::Value::object();
        json::Value list = json::Value::array();
        list.push(std::move(handler));
        group["hooks"] = std::move(list);
        json::Value& arr = hooks[ev];
        if (!arr.is_array()) arr = json::Value::array();
        arr.push(std::move(group));
    }
    if (!save_settings(path, settings, existed)) return 1;
    printf("  hooks:    %zu events in %s\n", sizeof kHookEvents / sizeof *kHookEvents, path.c_str());

    if (has_flag(argc, argv, "--no-service")) {
        printf("  service:  skipped (--no-service); the hook autostarts the daemon\n");
    } else if (!install_service(exe)) {
        fprintf(stderr, "warning: service setup failed; the hook will autostart the daemon instead\n");
    }
    printf("Done. Running Claude Code sessions pick the hooks up on their next start\n"
           "(or after opening /hooks). Check with: agent-indicator status\n");
    return 0;
}

int cmd_uninstall(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::string path = settings_path();
    json::Value settings;
    bool existed = false;
    if (!load_settings(path, settings, existed)) return 1;
    if (existed) {
        int n = strip_our_hooks(settings);
        if (n && !save_settings(path, settings, existed)) return 1;
        printf("  hooks:    removed %d handlers from %s\n", n, path.c_str());
    }
    remove_service();
    printf("  service:  removed\n");
    printf("Config (%s) and state (%s) were left in place.\n", plat::config_dir().c_str(), plat::state_dir().c_str());
    return 0;
}

int cmd_status(int argc, char** argv) {
    std::string text;
    if (!plat::read_file(plat::state_dir() + "/status.json", text)) {
        printf("daemon: no status yet (is it running?)\n");
        return 1;
    }
    if (has_flag(argc, argv, "--json")) { fputs(text.c_str(), stdout); return 0; }
    json::Value v;
    try { v = json::parse(text); } catch (...) { printf("daemon: unreadable status\n"); return 1; }
    auto num = [](const json::Value* o, const char* k) -> long long {
        const json::Value* x = o ? o->find(k) : nullptr;
        return x ? atoll(x->str().c_str()) : 0;
    };
    const long long age = (plat::wall_ms() - num(&v, "updated")) / 1000;
    const std::string dev = v.get_string("device");
    printf("daemon:  %s (status %llds old)\n", age < 15 ? "running" : "NOT UPDATING", age);
    printf("device:  %s\n", dev.empty() ? "not connected" : (dev + "  " + v.get_string("device_ident")).c_str());
    if (const json::Value* p = v.find("panels"); p && p->is_array()) {
        printf("panels: ");
        for (const auto& x : p->items()) printf(" [%s]", x.str().empty() ? "-" : x.str().c_str());
        printf("%s\n", (v.find("screensaver") && v.find("screensaver")->as_bool()) ? "  (screensaver)" : "");
    }
    const json::Value* t = v.find("today");
    printf("today:   agent time %s, busy %s, avg wait %s (%lld), avg idle %s (%lld), %lld tool calls, %lld/min\n",
           fmt_ms(num(t, "agent_work_ms")).c_str(), fmt_ms(num(t, "busy_wall_ms")).c_str(),
           fmt_ms(num(t, "avg_wait_ms")).c_str(), num(t, "waits"), fmt_ms(num(t, "avg_idle_ms")).c_str(),
           num(t, "idles"), num(t, "tool_calls"), num(t, "actions_per_min"));
    const json::Value* s = v.find("sessions");
    printf("sessions: %zu\n", s && s->is_array() ? s->items().size() : 0);
    int i = 0;
    if (s && s->is_array()) {
        for (const auto& o : s->items()) {
            std::string st = o.get_string("state");
            if (!o.get_string("wait").empty()) st += "/" + o.get_string("wait");
            if (!o.get_string("error").empty()) st += "/" + o.get_string("error");
            printf("  %2d  %-22s %6s  age %-5s pid %-7lld %s\n", ++i, st.c_str(), fmt_ms(num(&o, "in_state_ms")).c_str(),
                   fmt_ms(num(&o, "age_ms")).c_str(), num(&o, "pid"), o.get_string("cwd").c_str());
        }
    }
    // Feeds from other machines (shown after the local sessions on the bar).
    if (const json::Value* rs = v.find("remotes"); rs && rs->is_array()) {
        for (const auto& r : rs->items()) {
            const bool live = r.find("live") && r.find("live")->as_bool();
            const json::Value* ss = r.find("sessions");
            const size_t n = ss && ss->is_array() ? ss->items().size() : 0;
            printf("remote %s (port %s): %s", r.get_string("host").c_str(), r.get_string("target").c_str(),
                   live ? "" : "no feed (SSH forward not up, or no daemon on the far side)\n");
            if (!live) continue;
            printf("%zu session%s\n", n, n == 1 ? "" : "s");
            for (const auto& o : ss->items()) {
                std::string st = o.get_string("state");
                if (!o.get_string("wait").empty()) st += "/" + o.get_string("wait");
                printf("  %2d  %-22s %6s\n", ++i, st.c_str(), fmt_ms(num(&o, "in_state_ms")).c_str());
            }
        }
    }
    printf("broadcast: %lld listener%s\n", num(&v, "broadcast_clients"), num(&v, "broadcast_clients") == 1 ? "" : "s");
    return 0;
}

int cmd_raw(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: agent-indicator raw <device command...>\n"); return 2; }
    std::string line;
    for (int i = 2; i < argc; ++i) { if (i > 2) line += ' '; line += argv[i]; }
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = plat::socket_path();
    memcpy(addr.sun_path, path.c_str(), std::min(path.size() + 1, sizeof addr.sun_path - 1));
    std::string msg = "CTL1\traw\t" + line;
    bool ok = sendto(fd, msg.data(), msg.size(), 0, (sockaddr*)&addr, sizeof addr) == (ssize_t)msg.size();
    close(fd);
    if (!ok) { fprintf(stderr, "daemon not reachable at %s\n", path.c_str()); return 1; }
    return 0;
}
