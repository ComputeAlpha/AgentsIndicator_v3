// ~/.config/agent-indicator/config  (key = value, '#' comments)
#pragma once
#include <string>
#include <vector>

#include "tracker.hpp"

struct Config {
    std::string serial_port;          // empty = auto-detect
    int brightness = 38;              // 0..255 (38 = 15 %)
    int current_budget_ma = 900;      // whole matrix
    int screensaver_min = 30;         // nothing active this long -> starry night
    int panel_rotate_s = 5;           // pages alternate this often
    bool autostart = true;            // hook launches the daemon if it isn't running
    // One comma-separated page list per digit panel, left to right.
    std::vector<std::vector<std::string>> panels = {
        {"longest_wait", "longest_work"},
        {"agent_time", "actions_rate"},
        {"avg_wait", "avg_idle"},
        {"sessions"},
    };
    TrackerConfig tracker;

    static Config load(std::string* warnings = nullptr);
    static std::string default_text();
};
