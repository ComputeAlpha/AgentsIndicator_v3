// Session state machine + metrics. Pure logic: every call takes `now` (ms on
// plat::mono_ms's clock), so tests can drive time explicitly.
//
// Timing rules that keep multi-session numbers honest:
//  * Each session owns its own timestamps, keyed by Claude's session_id.
//    Nothing is keyed by bar position, so a slot shifting left when another
//    session closes can never hand one session's timer to another.
//  * Only the daemon's clock is used. Hook payloads carry no time we trust.
//  * Interval metrics are closed out on every state transition (exact), and
//    the "today" accumulators are split at local midnight.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

namespace json { class Value; }

enum class SessState { Idle, Working, Waiting, Done, Error };
enum class WaitKind { None, Permission, TurnEnd };

const char* state_name(SessState s);

struct HookEvent {
    std::string name;        // hook_event_name
    std::string session;     // session_id
    std::string agent_id;    // non-empty when fired from a subagent
    std::string detail;      // source / reason / notification_type / error
    std::string tool;        // tool_name
    std::string cwd;
    pid_t pid = 0;           // Claude Code process
};

struct Session {
    std::string id;
    std::string cwd;
    pid_t pid = 0;
    uint64_t pid_start = 0;
    uint64_t order = 0;      // stable left-to-right position on the bar
    SessState state = SessState::Idle;
    WaitKind wait = WaitKind::None;
    std::string error;       // StopFailure error / "crashed"
    int64_t created = 0;
    int64_t state_since = 0;
    int64_t work_since = 0;  // start of the current working streak
    int64_t last_event = 0;
    int64_t remove_at = 0;   // for Done / Error lingering
    int64_t metric_mark = 0; // start of the not-yet-accounted part of this state
    int inflight = 0;        // main-thread tools started but not finished
};

struct TrackerConfig {
    int64_t turn_wait_to_idle_ms = 10 * 60'000;  // finished turn: yellow -> red
    int64_t done_linger_ms = 4'000;              // green flash after close
    int64_t crash_linger_ms = 120'000;           // red flashing after a crash
    int64_t error_hold_ms = 10 * 60'000;         // API error flashing, then idle
    int64_t working_stale_ms = 20 * 60'000;      // cyan with no events/tools: interrupted
    int64_t stale_ms = 24 * 3600'000LL;          // no events, PID unknown
    int64_t rate_window_ms = 5 * 60'000;         // actions/min averaging
};

struct DayMetrics {
    int day = 0;                     // YYYYMMDD
    int64_t agent_work_ms = 0;       // sum over sessions of time spent working
    int64_t busy_wall_ms = 0;        // wall time with >= 1 session working
    int64_t wait_total_ms = 0;       // finished yellow intervals
    int64_t wait_count = 0;
    int64_t idle_total_ms = 0;       // finished red intervals
    int64_t idle_count = 0;
    int64_t tool_calls = 0;
    int64_t sessions_started = 0;
};

class Tracker {
public:
    explicit Tracker(TrackerConfig cfg = {}) : cfg_(cfg) {}

    using AliveFn = std::function<bool(pid_t, uint64_t)>;

    void on_event(const HookEvent& ev, int64_t now, uint64_t pid_start = 0);
    // Periodic housekeeping. `alive(pid, start)` reports process liveness.
    void tick(int64_t now, const AliveFn& alive);
    // Split today's accumulators at local midnight.
    void roll_day(int new_day, int64_t now);

    // Sessions in bar order (oldest first).
    std::vector<const Session*> ordered() const;
    const DayMetrics& metrics() const { return day_; }
    DayMetrics live_metrics(int64_t now) const;   // includes ongoing intervals
    double actions_per_min(int64_t now) const;
    int64_t last_activity() const { return last_activity_; }
    bool empty() const { return sessions_.empty(); }
    void mark_activity(int64_t now) { last_activity_ = now; }

    // Persistence across daemon restarts. Times are converted between the
    // monotonic clock and wall clock using (now_mono, now_wall).
    json::Value save(int64_t now_mono, int64_t now_wall) const;
    void load(const json::Value& v, int64_t now_mono, int64_t now_wall, int today, const AliveFn& alive);

private:
    Session& get_or_create(const HookEvent& ev, int64_t now, uint64_t pid_start);
    void transition(Session& s, SessState to, WaitKind wk, int64_t now);
    void account(Session& s, int64_t now);   // close out time in current state

    TrackerConfig cfg_;
    std::map<std::string, Session> sessions_;
    uint64_t next_order_ = 1;
    DayMetrics day_;
    int64_t busy_mark_ = 0;          // start of the current "any working" span
    std::deque<int64_t> tool_times_;
    int64_t last_activity_ = 0;      // last time anything was non-idle
};

