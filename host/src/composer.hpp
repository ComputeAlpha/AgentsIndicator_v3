// Tracker state -> one device frame line ("F <seq> ..."), see
// HW_firmware/AgentIndicatorv3_HW/src/protocol.hpp for the format.
#pragma once
#include <cstdint>
#include <string>

#include "config.hpp"
#include "tracker.hpp"

struct PanelText {
    std::string token;   // "<value>:<unit>:<colour>"
    std::string label;   // for `status`
};

// Duration -> (value, unit char) with s < 60 s, m < 60 min, else h (max 99).
void format_duration(int64_t ms, int& value, char& unit);

class Composer {
public:
    explicit Composer(const Config& cfg) : cfg_(cfg) {}
    // Body of the frame without "F <seq>". Deterministic for a given `now`.
    std::string compose(const Tracker& t, int64_t now, PanelText out_panels[4]) const;
    bool screensaver(const Tracker& t, int64_t now) const;

private:
    bool page(const std::string& name, const Tracker& t, const DayMetrics& m, int64_t now, PanelText& out) const;
    const Config& cfg_;
};
