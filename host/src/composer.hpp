// Tracker state -> one device frame line ("F <seq> ..."), see
// HW_firmware/AgentIndicatorv3_HW/src/protocol.hpp for the format.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "tracker.hpp"

// What the display is composed from: the local tracker, optionally merged
// with session feeds broadcast by daemons on other machines (see feed.hpp).
// All times are durations relative to "now", so views from different
// machines combine without sharing a clock.
struct ViewSession {
    SessState state = SessState::Idle;
    WaitKind wait = WaitKind::None;
    int64_t in_state_ms = 0;   // time in the current state
    int64_t streak_ms = 0;     // current working streak
};

struct View {
    std::vector<ViewSession> sessions;   // bar order
    DayMetrics day;                      // live (includes ongoing intervals)
    double actions_per_min = 0;
    int64_t inactive_ms = 0;             // since anything was last non-idle
};

View make_view(const Tracker& t, int64_t now);
// Append `other` after `into` (bar order) and add up its metrics.
void merge_view(View& into, const View& other);

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
    // `now` only drives the page-flip beat.
    std::string compose(const View& v, int64_t now, PanelText out_panels[4]) const;
    bool screensaver(const View& v) const;
    std::string compose(const Tracker& t, int64_t now, PanelText out_panels[4]) const {
        return compose(make_view(t, now), now, out_panels);
    }
    bool screensaver(const Tracker& t, int64_t now) const { return screensaver(make_view(t, now)); }

private:
    bool page(const std::string& name, const View& v, PanelText& out) const;
    const Config& cfg_;
};
