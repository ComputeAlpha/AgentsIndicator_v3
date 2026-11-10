// Host unit tests. No framework: CHECK prints and counts failures.
#include <cstdio>
#include <set>
#include <string>

#include "composer.hpp"
#include "feed.hpp"
#include <unistd.h>
#include "config.hpp"
#include "json.hpp"
#include "tracker.hpp"
#include "wire.hpp"

// The firmware's parser, compiled for the host: every frame the composer emits
// must be accepted by the exact code that runs on the device.
#include "../../HW_firmware/AgentIndicatorv3_HW/src/protocol.cpp"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) ++g_pass;                                                          \
        else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        auto _a = (a); auto _b = (b);                                                \
        if (_a == _b) ++g_pass;                                                      \
        else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)

static const auto kAlive = [](pid_t, uint64_t) { return true; };

static HookEvent ev(const char* name, const char* sid, pid_t pid = 0, const char* detail = "", const char* agent = "") {
    HookEvent e;
    e.name = name;
    e.session = sid;
    e.pid = pid;
    e.detail = detail;
    e.agent_id = agent;
    return e;
}

static const Session* find(const Tracker& t, const std::string& id) {
    for (auto* s : t.ordered())
        if (s->id == id) return s;
    return nullptr;
}

static void test_basic_lifecycle() {
    Tracker t;
    t.roll_day(20261107, 0);
    t.on_event(ev("SessionStart", "a", 0, "startup"), 1000);
    CHECK(find(t, "a")->state == SessState::Idle);
    t.on_event(ev("UserPromptSubmit", "a"), 2000);
    CHECK(find(t, "a")->state == SessState::Working);
    t.on_event(ev("PreToolUse", "a"), 3000);
    t.on_event(ev("PermissionRequest", "a"), 3100);
    CHECK(find(t, "a")->state == SessState::Waiting);
    CHECK(find(t, "a")->wait == WaitKind::Permission);
    t.on_event(ev("PostToolUse", "a"), 13100);   // approved after 10 s
    CHECK(find(t, "a")->state == SessState::Working);
    t.on_event(ev("Stop", "a"), 20000);
    CHECK(find(t, "a")->state == SessState::Waiting);
    CHECK(find(t, "a")->wait == WaitKind::TurnEnd);
    t.on_event(ev("SessionEnd", "a", 0, "prompt_input_exit"), 25000);
    CHECK(find(t, "a")->state == SessState::Done);
    t.tick(25000 + 3000, kAlive);
    CHECK(find(t, "a") != nullptr);              // green flash lingers
    t.tick(25000 + 4000, kAlive);
    CHECK(find(t, "a") == nullptr);

    const DayMetrics& m = t.metrics();
    // working: 2000..3100 and 13100..20000
    CHECK_EQ(m.agent_work_ms, (int64_t)(1100 + 6900));
    CHECK_EQ(m.wait_count, (int64_t)2);          // permission (10 s) + turn end (5 s)
    CHECK_EQ(m.wait_total_ms, (int64_t)(10000 + 5000));
    CHECK_EQ(m.idle_count, (int64_t)1);          // 1000..2000 before the first prompt
    CHECK_EQ(m.idle_total_ms, (int64_t)1000);
}

// The v2 bug class: timers must follow the session, not the bar position.
static void test_timers_follow_sessions_when_slots_shift() {
    Tracker t;
    t.roll_day(1, 0);
    t.on_event(ev("UserPromptSubmit", "A"), 0);       // slot 0
    t.on_event(ev("UserPromptSubmit", "B"), 10'000);  // slot 1
    t.on_event(ev("Stop", "B"), 70'000);              // B waits from 70 s
    t.on_event(ev("UserPromptSubmit", "C"), 100'000); // slot 2

    auto ord = t.ordered();
    CHECK_EQ(ord.size(), (size_t)3);
    CHECK_EQ(ord[0]->id, std::string("A"));

    // A closes: B and C shift left on the bar.
    t.on_event(ev("SessionEnd", "A"), 200'000);
    t.tick(200'000 + 5000, kAlive);
    ord = t.ordered();
    CHECK_EQ(ord.size(), (size_t)2);
    CHECK_EQ(ord[0]->id, std::string("B"));
    CHECK_EQ(ord[1]->id, std::string("C"));
    const int64_t now = 205'000;
    CHECK_EQ(now - ord[0]->state_since, (int64_t)135'000);   // B: waiting since 70 s
    CHECK_EQ(now - ord[1]->work_since, (int64_t)105'000);    // C: working since 100 s

    // Composer: longest wait is B's, longest work is C's (not A's old timer).
    Config cfg;
    cfg.panels = {{"longest_wait"}, {"longest_work"}, {"working"}, {"waiting"}};
    Composer comp(cfg);
    PanelText p[4];
    std::string body = comp.compose(t, now, p);
    CHECK_EQ(p[0].token, std::string("2:m:y"));   // 135 s -> 2 min
    CHECK_EQ(p[1].token, std::string("1:m:c"));   // 105 s -> 1 min
    CHECK_EQ(p[2].token, std::string("1:n:c"));
    CHECK_EQ(p[3].token, std::string("1:n:y"));
    CHECK_EQ(body.substr(0, 5), std::string("A YW "));
}

static void test_subagents_do_not_answer_for_human() {
    Tracker t;
    t.on_event(ev("UserPromptSubmit", "s"), 0);
    t.on_event(ev("Stop", "s"), 1000);
    t.on_event(ev("PreToolUse", "s", 0, "", "agent-123"), 2000);   // background subagent
    CHECK(find(t, "s")->state == SessState::Waiting);
    t.on_event(ev("PreToolUse", "s"), 3000);                       // main thread resumes
    CHECK(find(t, "s")->state == SessState::Working);
    t.on_event(ev("PermissionRequest", "s"), 4000);
    t.on_event(ev("SubagentStop", "s", 0, "", "agent-123"), 5000);
    CHECK(find(t, "s")->state == SessState::Waiting);
}

static void test_turn_wait_decays_to_idle_but_permission_does_not() {
    TrackerConfig c;
    c.turn_wait_to_idle_ms = 60'000;
    Tracker t(c);
    t.on_event(ev("UserPromptSubmit", "turn"), 0);
    t.on_event(ev("Stop", "turn"), 1000);
    t.on_event(ev("UserPromptSubmit", "perm"), 0);
    t.on_event(ev("Notification", "perm", 0, "permission_prompt"), 1000);
    t.on_event(ev("Notification", "turn", 0, "idle_prompt"), 30'000);   // ignored
    t.tick(61'000, kAlive);
    CHECK(find(t, "turn")->state == SessState::Idle);
    CHECK(find(t, "perm")->state == SessState::Waiting);
}

static void test_crash_vs_closed_terminal() {
    Tracker t;
    t.on_event(ev("UserPromptSubmit", "busy", 100), 0);
    t.on_event(ev("UserPromptSubmit", "quiet", 200), 0);
    t.on_event(ev("Stop", "quiet", 200), 500);
    t.tick(1000, [](pid_t, uint64_t) { return false; });
    CHECK(find(t, "busy")->state == SessState::Error);
    CHECK_EQ(find(t, "busy")->error, std::string("crashed"));
    CHECK(find(t, "quiet")->state == SessState::Done);
}

static void test_api_error_and_recovery() {
    Tracker t;
    t.on_event(ev("UserPromptSubmit", "e"), 0);
    t.on_event(ev("StopFailure", "e", 0, "rate_limit"), 1000);
    CHECK(find(t, "e")->state == SessState::Error);
    CHECK_EQ(find(t, "e")->error, std::string("rate_limit"));
    t.on_event(ev("UserPromptSubmit", "e"), 5000);
    CHECK(find(t, "e")->state == SessState::Working);
    CHECK(find(t, "e")->error.empty());
}

static void test_clear_keeps_slot() {
    Tracker t;
    t.on_event(ev("SessionStart", "first", 10), 0);
    t.on_event(ev("SessionStart", "second", 20), 0);
    t.on_event(ev("SessionEnd", "first", 10, "clear"), 1000);
    t.on_event(ev("SessionStart", "first-new", 10, "clear"), 1010);
    auto ord = t.ordered();
    CHECK_EQ(ord.size(), (size_t)2);
    CHECK_EQ(ord[0]->id, std::string("first-new"));
    CHECK_EQ(ord[1]->id, std::string("second"));
}

static void test_same_pid_live_sessions_coexist() {
    Tracker t;
    t.on_event(ev("SessionStart", "lead", 10), 0);
    t.on_event(ev("SessionStart", "mate", 10), 100);   // in-process teammate
    CHECK_EQ(t.ordered().size(), (size_t)2);
}

static void test_late_events_dont_resurrect() {
    Tracker t;
    t.on_event(ev("UserPromptSubmit", "z"), 0);
    t.on_event(ev("SessionEnd", "z"), 100);
    t.on_event(ev("PostToolUse", "z"), 150);
    CHECK(find(t, "z")->state == SessState::Done);
}

static void test_interrupt_and_stale_working() {
    TrackerConfig c;
    c.working_stale_ms = 10'000;
    Tracker t(c);
    t.on_event(ev("UserPromptSubmit", "i"), 0);
    t.on_event(ev("PreToolUse", "i"), 100);
    t.on_event(ev("PostToolUseFailure", "i", 0, "interrupt"), 500);
    CHECK(find(t, "i")->state == SessState::Waiting);

    t.on_event(ev("UserPromptSubmit", "long"), 0);
    t.on_event(ev("PreToolUse", "long"), 100);          // a 30-minute build
    t.tick(20'000, kAlive);
    CHECK(find(t, "long")->state == SessState::Working);   // tool in flight: not stale
    t.on_event(ev("PostToolUse", "long"), 30'000);
    t.tick(45'000, kAlive);
    CHECK(find(t, "long")->state == SessState::Idle);      // interrupted mid-thought
}

static void test_day_rollover_and_busy_time() {
    Tracker t;
    t.roll_day(20261107, 0);
    t.on_event(ev("UserPromptSubmit", "a"), 0);
    t.on_event(ev("UserPromptSubmit", "b"), 0);
    // two agents working for 100 s = 200 s agent time, 100 s wall
    DayMetrics m = t.live_metrics(100'000);
    CHECK_EQ(m.agent_work_ms, (int64_t)200'000);
    CHECK_EQ(m.busy_wall_ms, (int64_t)100'000);
    t.roll_day(20261108, 100'000);
    m = t.live_metrics(160'000);
    CHECK_EQ(m.agent_work_ms, (int64_t)120'000);
    CHECK_EQ(m.busy_wall_ms, (int64_t)60'000);
    CHECK_EQ(m.day, 20261108);
}

static void test_persistence_roundtrip() {
    Tracker t;
    t.roll_day(7, 0);
    t.on_event(ev("UserPromptSubmit", "p", 42), 1000);
    t.on_event(ev("Stop", "p", 42), 61'000);
    json::Value saved = t.save(100'000, 5'000'000);
    std::string text = saved.dump(2);

    // daemon restarts: mono clock now 500'000, wall clock moved by the same 400 s
    Tracker u;
    u.load(json::parse(text), 500'000, 5'400'000, 7, kAlive);
    const Session* s = find(u, "p");
    CHECK(s != nullptr);
    CHECK(s->state == SessState::Waiting);
    CHECK_EQ(500'000 - s->state_since, (int64_t)(500'000 - 61'000));
    CHECK_EQ(u.metrics().agent_work_ms, (int64_t)60'000);

    Tracker dead;
    dead.load(json::parse(text), 500'000, 5'400'000, 7, [](pid_t, uint64_t) { return false; });
    CHECK(find(dead, "p") == nullptr);
}

static void test_format_and_frames_parse_on_device() {
    int v;
    char u;
    format_duration(59'999, v, u);  CHECK(v == 59 && u == 's');
    format_duration(60'000, v, u);  CHECK(v == 1 && u == 'm');
    format_duration(3'599'999, v, u); CHECK(v == 59 && u == 'm');
    format_duration(36 * 3600'000LL, v, u); CHECK(v == 36 && u == 'h');
    format_duration(500 * 3600'000LL, v, u); CHECK(v == 99 && u == 'h');

    // Fill the bar past 40 and run every page through the firmware parser.
    Tracker t;
    t.roll_day(1, 0);
    for (int i = 0; i < 45; ++i) {
        std::string id = "s" + std::to_string(i);
        t.on_event(ev("UserPromptSubmit", id.c_str()), 0);
        if (i % 3 == 0) t.on_event(ev("Stop", id.c_str()), 1000);
        if (i % 5 == 0) t.on_event(ev("PreToolUse", id.c_str()), 1500);
    }
    t.on_event(ev("StopFailure", "s1", 0, "overloaded"), 2000);
    Config cfg;
    Composer comp(cfg);
    for (int64_t now : {3'000LL, 130'000LL, 4'000'000LL}) {
        PanelText p[4];
        std::string body = comp.compose(t, now, p);
        disp::Model m;
        CHECK(proto::parse_frame((" 1 " + body).c_str(), m));
        CHECK_EQ((int)m.nslots, 40);
        CHECK(m.overflow);
    }
    // empty tracker -> "-" bar and blank panels
    Tracker empty;
    PanelText p[4];
    std::string body = comp.compose(empty, 0, p);
    disp::Model m;
    CHECK(proto::parse_frame((" 9 " + body).c_str(), m));
    CHECK_EQ((int)m.nslots, 0);
    CHECK(m.panels[0].value == disp::PanelValue::kBlank);
    // and the parser rejects junk
    CHECK(!proto::parse_frame(" 1 A WX 1:s:c 1:s:c 1:s:c 1:s:c", m));
    CHECK(!proto::parse_frame(" 1 A W 100:s:c 1:s:c 1:s:c 1:s:c", m));
    CHECK(!proto::parse_frame(" 1 A W 1:s:c", m));
}

static void test_screensaver() {
    Config cfg;
    cfg.screensaver_min = 30;
    Composer comp(cfg);
    Tracker t;
    t.on_event(ev("SessionStart", "x"), 0);
    CHECK(!comp.screensaver(t, 29 * 60'000));
    CHECK(comp.screensaver(t, 30 * 60'000));
    t.on_event(ev("UserPromptSubmit", "x"), 31 * 60'000);
    CHECK(!comp.screensaver(t, 90 * 60'000));   // working sessions never sleep the display
}

static void test_json_and_wire() {
    json::Value v = json::parse(R"({"b":1,"a":[true,null,"xé😀\n"],"n":-1.5e3})");
    CHECK_EQ(v.members()[0].first, std::string("b"));   // order kept
    CHECK_EQ(v.find("a")->items()[2].str(), std::string("x\xc3\xa9\xf0\x9f\x98\x80\n"));
    CHECK_EQ(v.find("n")->str(), std::string("-1.5e3"));
    json::Value again = json::parse(v.dump(2));
    CHECK_EQ(again.dump(0), v.dump(0));
    bool threw = false;
    try { json::parse("{\"a\":}"); } catch (...) { threw = true; }
    CHECK(threw);

    wire::Event e{"PreToolUse", "sid", "", "", "Bash", "/tmp/with\ttab", 1234};
    wire::Event d;
    CHECK(wire::decode(wire::encode(e), d));
    CHECK_EQ(d.pid, 1234);
    CHECK_EQ(d.cwd, std::string("/tmp/with tab"));
}

static void test_auto_compact_between_turns_keeps_state() {
    Tracker t;
    t.roll_day(20261107, 0);
    t.on_event(ev("SessionStart", "a", 0, "startup"), 1000);
    t.on_event(ev("UserPromptSubmit", "a"), 2000);
    t.on_event(ev("Stop", "a"), 5000);
    // Claude Code auto-compacts after the turn ended: no Stop follows.
    t.on_event(ev("PreCompact", "a", 0, "auto"), 6000);
    t.on_event(ev("PostCompact", "a", 0, "auto"), 56000);
    CHECK(find(t, "a")->state == SessState::Waiting);
    CHECK(find(t, "a")->wait == WaitKind::TurnEnd);
    // mid-turn auto compaction stays Working
    t.on_event(ev("UserPromptSubmit", "a"), 60000);
    t.on_event(ev("PreCompact", "a", 0, "auto"), 61000);
    t.on_event(ev("PostCompact", "a", 0, "auto"), 90000);
    CHECK(find(t, "a")->state == SessState::Working);
}

static void test_remote_feed() {
    // Server machine: one waiting session (12 s) and one working (streak 40 s).
    Tracker srv;
    srv.roll_day(20261110, 0);
    srv.on_event(ev("UserPromptSubmit", "r1"), 0);
    srv.on_event(ev("Stop", "r1"), 28'000);
    srv.on_event(ev("UserPromptSubmit", "r2"), 0);
    srv.on_event(ev("PreToolUse", "r2"), 30'000);
    const std::string line = feed::encode(make_view(srv, 40'000), "devbox");
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find("r1") == std::string::npos);   // no session ids leave the machine

    View rv;
    std::string host;
    CHECK(feed::decode(line, rv, host));
    CHECK_EQ(host, std::string("devbox"));
    CHECK_EQ(rv.sessions.size(), (size_t)2);
    CHECK(rv.sessions[0].state == SessState::Waiting && rv.sessions[0].wait == WaitKind::TurnEnd);
    CHECK_EQ(rv.sessions[0].in_state_ms, (int64_t)12'000);
    CHECK_EQ(rv.sessions[1].streak_ms, (int64_t)40'000);
    CHECK_EQ(rv.day.tool_calls, (int64_t)1);
    CHECK(!feed::decode("{\"v\":2,\"s\":[],\"d\":[0,0,0,0,0,0,0,0]}", rv, host));
    CHECK(!feed::decode("F 1 A W 1:s:c", rv, host));
    CHECK(!feed::decode("{\"v\":1,\"s\":[[\"Q\",\"\",0,0]],\"d\":[0,0,0,0,0,0,0,0]}", rv, host));

    // Client machine: one local idle session; remote ones are appended to the bar
    // and the longest wait is the remote's.
    Tracker loc;
    loc.roll_day(20261110, 0);
    loc.on_event(ev("SessionStart", "l1"), 0);
    View v = make_view(loc, 50'000);
    feed::decode(line, rv, host);
    merge_view(v, rv);
    Config cfg;
    Composer comp(cfg);
    PanelText p[4];
    std::string body = comp.compose(v, 0, p);   // beat 0: first page of each panel
    CHECK(body.rfind("A IYW ", 0) == 0);
    CHECK(p[0].token == "12:s:y");
    CHECK(p[3].token == "3:n:x");
    // A busy remote keeps the shared display out of the screensaver.
    CHECK(!comp.screensaver(v));

    // End to end over loopback: Server -> Remote.
    feed::Server server;
    int port = 0;
    for (int cand = 47391; cand < 47400 && !port; ++cand)
        if (server.listen(cand)) port = cand;
    CHECK(port != 0);
    feed::Remote remote(std::to_string(port));
    int64_t now = 0;
    for (int i = 0; i < 200 && !remote.live(now); ++i) {
        remote.poll(now);
        server.accept_pending();
        if (server.clients()) server.publish(line);
        usleep(2000);
        now += 2;
    }
    CHECK(remote.live(now));
    CHECK_EQ(remote.host(), std::string("devbox"));
    CHECK_EQ(remote.view(now).sessions.size(), (size_t)2);
    // Sessions age locally between updates and disappear when the feed stops.
    CHECK(remote.view(now + 1000).sessions[0].in_state_ms >= 13'000);
    server.close_all();
    remote.poll(now + 6000);
    CHECK(!remote.live(now + 6000));
    CHECK(remote.view(now + 6000).sessions.empty());
}

int main() {
    test_basic_lifecycle();
    test_timers_follow_sessions_when_slots_shift();
    test_subagents_do_not_answer_for_human();
    test_turn_wait_decays_to_idle_but_permission_does_not();
    test_crash_vs_closed_terminal();
    test_api_error_and_recovery();
    test_clear_keeps_slot();
    test_late_events_dont_resurrect();
    test_same_pid_live_sessions_coexist();
    test_interrupt_and_stale_working();
    test_day_rollover_and_busy_time();
    test_persistence_roundtrip();
    test_format_and_frames_parse_on_device();
    test_screensaver();
    test_auto_compact_between_turns_keeps_state();
    test_remote_feed();
    test_json_and_wire();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
