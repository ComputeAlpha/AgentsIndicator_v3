#include "config.hpp"

#include <cstdlib>
#include <sstream>

#include "platform.hpp"

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

std::string Config::default_text() {
    return R"(# AgentIndicator v3 host configuration. Restart the daemon after editing:
#   Linux:  systemctl --user restart agent-indicator
#   macOS:  launchctl kickstart -k gui/$(id -u)/com.agentindicator.daemon

# Serial device; leave empty to auto-detect the indicator over USB.
serial_port =

# LED brightness 0..255 (38 = 15 %), and the current cap for the whole matrix.
brightness = 38
current_budget_ma = 900

# Minutes with nothing working/waiting before the starry-night screensaver.
screensaver_min = 30

# A finished turn shows yellow (waiting for you) and turns red (idle) after this.
turn_wait_to_idle_min = 10
# How long an API error (rate limit, overload...) flashes before falling back to idle.
error_hold_min = 10
# A crashed session keeps flashing red for this long.
crash_linger_s = 120

# Digit panels, left to right. Each is a list of pages that alternate every
# panel_rotate_s seconds; pages with no data are skipped.
#   longest_wait  longest current yellow wait            (yellow, s/m/h dots)
#   longest_work  longest current working streak         (cyan, s/m/h dots)
#   agent_time    agent working time today, all sessions (rainbow, m/h dots)
#   busy_time     wall time today with >= 1 agent working(rainbow, m/h dots)
#   avg_wait      average yellow wait today              (yellow)
#   avg_idle      average idle gap today                 (red)
#   actions_rate  tool calls per minute, last 5 min      (magenta, no dots)
#   sessions      live sessions                          (rainbow, no dots)
#   working / waiting   number of sessions in that state
panel_rotate_s = 5
panel1 = longest_wait, longest_work
panel2 = agent_time, actions_rate
panel3 = avg_wait, avg_idle
panel4 = sessions

# Launch the daemon from the hook when no service is running it.
autostart = true
)";
}

Config Config::load(std::string* warnings) {
    Config c;
    std::string text;
    if (!plat::read_file(plat::config_dir() + "/config", text)) return c;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    auto warn = [&](const std::string& w) {
        if (warnings) *warnings += "config line " + std::to_string(lineno) + ": " + w + "\n";
    };
    while (std::getline(in, line)) {
        ++lineno;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { warn("expected key = value"); continue; }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto num = [&](int lo, int hi, int& dst) {
            char* end = nullptr;
            long n = strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || n < lo || n > hi) warn("bad value for " + k);
            else dst = (int)n;
        };
        int tmp = 0;
        if (k == "serial_port") c.serial_port = v;
        else if (k == "brightness") num(0, 255, c.brightness);
        else if (k == "current_budget_ma") num(100, 5000, c.current_budget_ma);
        else if (k == "screensaver_min") num(1, 24 * 60, c.screensaver_min);
        else if (k == "panel_rotate_s") num(1, 600, c.panel_rotate_s);
        else if (k == "turn_wait_to_idle_min") { tmp = 10; num(1, 24 * 60, tmp); c.tracker.turn_wait_to_idle_ms = tmp * 60'000LL; }
        else if (k == "error_hold_min") { tmp = 10; num(1, 24 * 60, tmp); c.tracker.error_hold_ms = tmp * 60'000LL; }
        else if (k == "crash_linger_s") { tmp = 120; num(1, 3600, tmp); c.tracker.crash_linger_ms = tmp * 1000LL; }
        else if (k == "autostart") c.autostart = (v == "true" || v == "1" || v == "yes");
        else if (k.size() == 6 && k.compare(0, 5, "panel") == 0 && k[5] >= '1' && k[5] <= '4')
            c.panels[(size_t)(k[5] - '1')] = split_list(v);
        else warn("unknown key " + k);
    }
    return c;
}
