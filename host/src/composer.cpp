#include "composer.hpp"

#include <algorithm>
#include <cmath>

void format_duration(int64_t ms, int& value, char& unit) {
    const int64_t s = std::max<int64_t>(0, ms / 1000);
    if (s < 60) { value = (int)s; unit = 's'; return; }
    const int64_t m = s / 60;
    if (m < 60) { value = (int)m; unit = 'm'; return; }
    value = (int)std::min<int64_t>(99, m / 60);
    unit = 'h';
}

namespace {

std::string token(int value, char unit, char colour) {
    return std::to_string(std::clamp(value, 0, 99)) + ":" + unit + ":" + colour;
}

std::string dur_token(int64_t ms, char colour) {
    int v;
    char u;
    format_duration(ms, v, u);
    return token(v, u, colour);
}

const char* unit_word(char u) { return u == 's' ? "s" : u == 'm' ? "min" : "h"; }

std::string dur_label(const char* what, int64_t ms) {
    int v;
    char u;
    format_duration(ms, v, u);
    return std::string(what) + " " + std::to_string(v) + unit_word(u);
}

}  // namespace

bool Composer::page(const std::string& name, const Tracker& t, const DayMetrics& m, int64_t now,
                    PanelText& out) const {
    auto longest = [&](SessState st, bool streak) -> int64_t {
        int64_t best = -1;
        for (const Session* s : t.ordered())
            if (s->state == st) best = std::max(best, now - (streak ? s->work_since : s->state_since));
        return best;
    };
    auto count = [&](SessState st) {
        int n = 0;
        for (const Session* s : t.ordered()) n += s->state == st;
        return n;
    };

    if (name == "longest_wait") {
        int64_t ms = longest(SessState::Waiting, false);
        if (ms < 0) return false;
        out = {dur_token(ms, 'y'), dur_label("longest wait", ms)};
    } else if (name == "longest_work") {
        int64_t ms = longest(SessState::Working, true);
        if (ms < 0) return false;
        out = {dur_token(ms, 'c'), dur_label("longest working", ms)};
    } else if (name == "agent_time" || name == "busy_time") {
        int64_t ms = name == "agent_time" ? m.agent_work_ms : m.busy_wall_ms;
        if (ms < 60'000) return false;   // show from the first full minute
        out = {dur_token(ms, 'x'), dur_label(name == "agent_time" ? "agent time today" : "busy time today", ms)};
    } else if (name == "avg_wait") {
        if (!m.wait_count) return false;
        int64_t ms = m.wait_total_ms / m.wait_count;
        out = {dur_token(ms, 'y'), dur_label("avg wait", ms)};
    } else if (name == "avg_idle") {
        if (!m.idle_count) return false;
        int64_t ms = m.idle_total_ms / m.idle_count;
        out = {dur_token(ms, 'r'), dur_label("avg idle", ms)};
    } else if (name == "actions_rate") {
        double r = t.actions_per_min(now);
        if (r <= 0.0) return false;
        int v = (int)std::lround(r);
        if (v < 1) v = 1;
        out = {token(v, 'n', 'm'), "actions/min " + std::to_string(v)};
    } else if (name == "sessions") {
        int n = 0;
        for (const Session* s : t.ordered()) n += s->state != SessState::Done;
        if (!n) return false;
        out = {token(n, 'n', 'x'), "sessions " + std::to_string(n)};
    } else if (name == "working" || name == "waiting") {
        int n = count(name == "working" ? SessState::Working : SessState::Waiting);
        if (!n) return false;
        out = {token(n, 'n', name == "working" ? 'c' : 'y'), name + " " + std::to_string(n)};
    } else {
        return false;
    }
    return true;
}

bool Composer::screensaver(const Tracker& t, int64_t now) const {
    for (const Session* s : t.ordered())
        if (s->state != SessState::Idle) return false;
    return now - t.last_activity() >= (int64_t)cfg_.screensaver_min * 60'000;
}

std::string Composer::compose(const Tracker& t, int64_t now, PanelText out_panels[4]) const {
    const DayMetrics m = t.live_metrics(now);
    const bool starry = screensaver(t, now);

    std::string bar;
    for (const Session* s : t.ordered()) {
        if (bar.size() >= 40) { bar += '+'; break; }
        switch (s->state) {
        case SessState::Working: bar += 'W'; break;
        case SessState::Waiting: bar += 'Y'; break;
        case SessState::Idle: bar += 'I'; break;
        case SessState::Done: bar += 'D'; break;
        case SessState::Error: bar += 'E'; break;
        }
    }
    if (bar.empty()) bar = "-";

    // All panels flip pages on the same wall-clock beat.
    const int64_t beat = now / (std::max(1, cfg_.panel_rotate_s) * 1000LL);
    std::string body = std::string(starry ? "S " : "A ") + bar;
    for (int p = 0; p < 4; ++p) {
        PanelText chosen{"_:n:k", ""};
        const auto& pages = cfg_.panels[(size_t)p];
        std::vector<PanelText> avail;
        for (const auto& name : pages) {
            PanelText pt;
            if (page(name, t, m, now, pt)) avail.push_back(pt);
        }
        if (!avail.empty()) chosen = avail[(size_t)(beat % (int64_t)avail.size())];
        out_panels[p] = chosen;
        body += " " + chosen.token;
    }
    return body;
}
