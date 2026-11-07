#include "tracker.hpp"

#include <algorithm>

#include "json.hpp"

const char* state_name(SessState s) {
    switch (s) {
    case SessState::Idle: return "idle";
    case SessState::Working: return "working";
    case SessState::Waiting: return "waiting";
    case SessState::Done: return "done";
    case SessState::Error: return "error";
    }
    return "?";
}

namespace {

bool is_permission_notification(const std::string& t) {
    return t == "permission_prompt" || t == "elicitation_dialog" || t == "elicitation_url_dialog" ||
           t == "agent_needs_input";
}

bool any_working(const std::map<std::string, Session>& m) {
    for (const auto& kv : m)
        if (kv.second.state == SessState::Working) return true;
    return false;
}

}  // namespace

// ------------------------------------------------------------ bookkeeping

void Tracker::account(Session& s, int64_t now) {
    if (s.state == SessState::Working && now > s.metric_mark) day_.agent_work_ms += now - s.metric_mark;
    s.metric_mark = now;
}

void Tracker::transition(Session& s, SessState to, WaitKind wk, int64_t now) {
    if (s.state == to && s.wait == wk) return;
    const bool was_busy = any_working(sessions_);

    account(s, now);
    if (s.state == SessState::Waiting && to == SessState::Waiting) {
        // permission prompt -> finished turn (or back): one continuous yellow span
        s.wait = wk;
        return;
    }
    const int64_t span = now - s.state_since;
    if (s.state == SessState::Waiting && (to == SessState::Working || to == SessState::Idle || to == SessState::Done)) {
        day_.wait_total_ms += span;
        ++day_.wait_count;
    }
    if (s.state == SessState::Idle && to == SessState::Working && span > 0) {
        day_.idle_total_ms += span;
        ++day_.idle_count;
    }
    if (to == SessState::Working && s.state != SessState::Working) s.work_since = now;
    if (to != SessState::Working) s.inflight = 0;

    s.state = to;
    s.wait = wk;
    s.state_since = now;
    s.metric_mark = now;
    if (to != SessState::Idle) last_activity_ = now;

    const bool is_busy = any_working(sessions_);
    if (!was_busy && is_busy) busy_mark_ = now;
    if (was_busy && !is_busy && now > busy_mark_) day_.busy_wall_ms += now - busy_mark_;
}

Session& Tracker::get_or_create(const HookEvent& ev, int64_t now, uint64_t pid_start) {
    auto it = sessions_.find(ev.session);
    if (it != sessions_.end()) return it->second;

    uint64_t order = 0;
    if (ev.pid > 0) {
        // Same Claude process under a new session id right after the old one
        // ended (/clear, /resume): take over its slot so the bar doesn't
        // shuffle. Live same-PID sessions (in-process teammates) keep theirs.
        for (auto jt = sessions_.begin(); jt != sessions_.end(); ++jt) {
            if (jt->second.pid == ev.pid && jt->second.state == SessState::Done) {
                order = jt->second.order;
                sessions_.erase(jt);
                break;
            }
        }
    }
    Session s;
    s.id = ev.session;
    s.cwd = ev.cwd;
    s.pid = ev.pid;
    s.pid_start = pid_start;
    s.order = order ? order : next_order_++;
    s.created = s.state_since = s.metric_mark = s.last_event = now;
    ++day_.sessions_started;
    last_activity_ = now;
    return sessions_.emplace(s.id, std::move(s)).first->second;
}

// ------------------------------------------------------------ events

void Tracker::on_event(const HookEvent& ev, int64_t now, uint64_t pid_start) {
    if (ev.session.empty() || ev.name.empty()) return;
    const std::string& n = ev.name;

    // Late events after a session ended must not resurrect it.
    if (n != "SessionStart") {
        auto it = sessions_.find(ev.session);
        if (it != sessions_.end() && it->second.state == SessState::Done) return;
    }

    Session& s = get_or_create(ev, now, pid_start);
    s.last_event = now;
    if (!ev.cwd.empty()) s.cwd = ev.cwd;
    if (s.pid == 0 && ev.pid > 0) { s.pid = ev.pid; s.pid_start = pid_start; }

    const bool sub = !ev.agent_id.empty();
    auto activity = [&] {
        if (s.state == SessState::Waiting && sub) return;   // a background agent can't answer for the human
        if (s.state == SessState::Error && sub) return;
        transition(s, SessState::Working, WaitKind::None, now);
    };

    if (n == "SessionStart") {
        if (s.state == SessState::Done || s.state == SessState::Error) {
            s.error.clear();
            transition(s, SessState::Idle, WaitKind::None, now);
        }
    } else if (n == "SessionEnd") {
        s.error.clear();
        transition(s, SessState::Done, WaitKind::None, now);
        s.remove_at = now + cfg_.done_linger_ms;
    } else if (n == "UserPromptSubmit" || n == "UserPromptExpansion") {
        s.error.clear();
        transition(s, SessState::Working, WaitKind::None, now);
    } else if (n == "PermissionRequest" || n == "Elicitation") {
        transition(s, SessState::Waiting, WaitKind::Permission, now);
    } else if (n == "Notification") {
        if (is_permission_notification(ev.detail)) transition(s, SessState::Waiting, WaitKind::Permission, now);
    } else if (n == "Stop") {
        if (!sub) transition(s, SessState::Waiting, WaitKind::TurnEnd, now);
    } else if (n == "StopFailure") {
        s.error = ev.detail.empty() ? "unknown" : ev.detail;
        transition(s, SessState::Error, WaitKind::None, now);
        s.remove_at = 0;
    } else if (n == "PreToolUse") {
        ++day_.tool_calls;
        tool_times_.push_back(now);
        activity();
        if (!sub && s.state == SessState::Working) ++s.inflight;
    } else if (n == "PostToolUse" || n == "PostToolUseFailure" || n == "PermissionDenied") {
        if (!sub && s.inflight > 0) --s.inflight;
        if (n == "PostToolUseFailure" && ev.detail == "interrupt" && !sub) {
            // user hit Esc mid-tool: Claude stops and waits for them
            transition(s, SessState::Waiting, WaitKind::TurnEnd, now);
        } else {
            activity();
        }
    } else if (n == "PreCompact" || n == "PostCompact") {
        if (ev.detail == "manual") {
            // /compact typed by the user: busy while it runs, then back to them
            transition(s, n == "PreCompact" ? SessState::Working : SessState::Idle, WaitKind::None, now);
        }
        // Auto compaction is housekeeping, not work. Mid-turn the session is
        // already Working; after a turn has ended (Claude Code also compacts
        // then) no Stop follows, so treating it as activity would leave the
        // session stuck on cyan. Keep whatever state it was in.
    } else if (n == "SubagentStart" || n == "SubagentStop" || n == "PostToolBatch" || n == "ElicitationResult") {
        activity();
    }
}

void Tracker::tick(int64_t now, const AliveFn& alive) {
    std::vector<std::string> erase;
    for (auto& [id, s] : sessions_) {
        switch (s.state) {
        case SessState::Done:
            if (now >= s.remove_at) erase.push_back(id);
            continue;
        case SessState::Error:
            if (s.error == "crashed") {
                if (now >= s.remove_at) erase.push_back(id);
                continue;
            }
            if (now - s.state_since >= cfg_.error_hold_ms) {
                s.error.clear();
                transition(s, SessState::Idle, WaitKind::None, now);
            }
            break;
        case SessState::Waiting:
            if (s.wait == WaitKind::TurnEnd && now - s.state_since >= cfg_.turn_wait_to_idle_ms)
                transition(s, SessState::Idle, WaitKind::None, now);
            break;
        case SessState::Working:
            if (s.inflight == 0 && now - s.last_event >= cfg_.working_stale_ms)
                transition(s, SessState::Idle, WaitKind::None, now);
            break;
        case SessState::Idle: break;
        }

        if (s.pid > 0 && !alive(s.pid, s.pid_start)) {
            // Process gone without SessionEnd. Mid-work or mid-prompt that's a
            // crash; otherwise the user just closed the terminal.
            const bool mid_work = s.state == SessState::Working ||
                                  (s.state == SessState::Waiting && s.wait == WaitKind::Permission);
            if (mid_work) {
                s.error = "crashed";
                transition(s, SessState::Error, WaitKind::None, now);
                s.remove_at = now + cfg_.crash_linger_ms;
            } else {
                transition(s, SessState::Done, WaitKind::None, now);
                s.remove_at = now + cfg_.done_linger_ms;
            }
        } else if (s.pid <= 0 && now - s.last_event >= cfg_.stale_ms) {
            transition(s, SessState::Done, WaitKind::None, now);
            erase.push_back(id);
        }
    }
    for (const auto& id : erase) sessions_.erase(id);
    while (!tool_times_.empty() && now - tool_times_.front() > cfg_.rate_window_ms) tool_times_.pop_front();
}

void Tracker::roll_day(int new_day, int64_t now) {
    if (day_.day == new_day) return;
    for (auto& kv : sessions_) account(kv.second, now);
    // Open yellow/red spans are attributed to the day they end in.
    day_ = DayMetrics{};
    day_.day = new_day;
    busy_mark_ = now;
}

// ------------------------------------------------------------ queries

std::vector<const Session*> Tracker::ordered() const {
    std::vector<const Session*> out;
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_) out.push_back(&kv.second);
    std::sort(out.begin(), out.end(), [](const Session* a, const Session* b) { return a->order < b->order; });
    return out;
}

DayMetrics Tracker::live_metrics(int64_t now) const {
    DayMetrics m = day_;
    bool busy = false;
    for (const auto& kv : sessions_) {
        const Session& s = kv.second;
        if (s.state == SessState::Working) {
            busy = true;
            if (now > s.metric_mark) m.agent_work_ms += now - s.metric_mark;
        }
    }
    if (busy && now > busy_mark_) m.busy_wall_ms += now - busy_mark_;
    return m;
}

double Tracker::actions_per_min(int64_t now) const {
    int64_t n = 0;
    for (int64_t t : tool_times_)
        if (now - t <= cfg_.rate_window_ms) ++n;
    return (double)n * 60'000.0 / (double)cfg_.rate_window_ms;
}

// ------------------------------------------------------------ persistence

json::Value Tracker::save(int64_t now_mono, int64_t now_wall) const {
    auto to_wall = [&](int64_t t) { return json::Value::number((long long)(t ? now_wall - (now_mono - t) : 0)); };
    json::Value root = json::Value::object();
    root["version"] = json::Value::number(1LL);
    root["next_order"] = json::Value::number((long long)next_order_);

    DayMetrics m = live_metrics(now_mono);
    json::Value d = json::Value::object();
    d["day"] = json::Value::number((long long)m.day);
    d["agent_work_ms"] = json::Value::number((long long)m.agent_work_ms);
    d["busy_wall_ms"] = json::Value::number((long long)m.busy_wall_ms);
    d["wait_total_ms"] = json::Value::number((long long)m.wait_total_ms);
    d["wait_count"] = json::Value::number((long long)m.wait_count);
    d["idle_total_ms"] = json::Value::number((long long)m.idle_total_ms);
    d["idle_count"] = json::Value::number((long long)m.idle_count);
    d["tool_calls"] = json::Value::number((long long)m.tool_calls);
    d["sessions_started"] = json::Value::number((long long)m.sessions_started);
    root["day"] = d;

    json::Value arr = json::Value::array();
    for (const Session* s : ordered()) {
        if (s->state == SessState::Done) continue;
        json::Value o = json::Value::object();
        o["id"] = json::Value::string(s->id);
        o["cwd"] = json::Value::string(s->cwd);
        o["pid"] = json::Value::number((long long)s->pid);
        o["pid_start"] = json::Value::number((long long)s->pid_start);
        o["order"] = json::Value::number((long long)s->order);
        o["state"] = json::Value::number((long long)s->state);
        o["wait"] = json::Value::number((long long)s->wait);
        o["error"] = json::Value::string(s->error);
        o["inflight"] = json::Value::number((long long)s->inflight);
        o["created"] = to_wall(s->created);
        o["state_since"] = to_wall(s->state_since);
        o["work_since"] = to_wall(s->work_since);
        o["last_event"] = to_wall(s->last_event);
        o["remove_at"] = to_wall(s->remove_at);
        arr.push(std::move(o));
    }
    root["sessions"] = std::move(arr);
    return root;
}

void Tracker::load(const json::Value& v, int64_t now_mono, int64_t now_wall, int today, const AliveFn& alive) {
    auto num = [](const json::Value& o, const char* k) -> long long {
        const json::Value* x = o.find(k);
        return x && x->type() == json::Value::Type::Number ? std::stoll(x->str()) : 0;
    };
    auto to_mono = [&](long long w) -> int64_t { return w ? now_mono - (now_wall - (int64_t)w) : 0; };

    day_ = DayMetrics{};
    day_.day = today;
    if (const json::Value* d = v.find("day"); d && d->is_object() && num(*d, "day") == today) {
        day_.agent_work_ms = num(*d, "agent_work_ms");
        day_.busy_wall_ms = num(*d, "busy_wall_ms");
        day_.wait_total_ms = num(*d, "wait_total_ms");
        day_.wait_count = num(*d, "wait_count");
        day_.idle_total_ms = num(*d, "idle_total_ms");
        day_.idle_count = num(*d, "idle_count");
        day_.tool_calls = num(*d, "tool_calls");
        day_.sessions_started = num(*d, "sessions_started");
    }
    next_order_ = std::max<uint64_t>(1, (uint64_t)num(v, "next_order"));

    sessions_.clear();
    if (const json::Value* arr = v.find("sessions"); arr && arr->is_array()) {
        for (const json::Value& o : arr->items()) {
            Session s;
            s.id = o.get_string("id");
            if (s.id.empty()) continue;
            s.cwd = o.get_string("cwd");
            s.pid = (pid_t)num(o, "pid");
            s.pid_start = (uint64_t)num(o, "pid_start");
            if (s.pid > 0 && !alive(s.pid, s.pid_start)) continue;   // ended while we were down
            s.order = (uint64_t)num(o, "order");
            s.state = (SessState)num(o, "state");
            s.wait = (WaitKind)num(o, "wait");
            s.error = o.get_string("error");
            s.inflight = (int)num(o, "inflight");
            s.created = to_mono(num(o, "created"));
            s.state_since = to_mono(num(o, "state_since"));
            s.work_since = to_mono(num(o, "work_since"));
            s.last_event = to_mono(num(o, "last_event"));
            s.remove_at = to_mono(num(o, "remove_at"));
            s.metric_mark = now_mono;   // daemon downtime is not counted as work
            next_order_ = std::max(next_order_, s.order + 1);
            sessions_.emplace(s.id, std::move(s));
        }
    }
    busy_mark_ = now_mono;
    last_activity_ = now_mono;
}
