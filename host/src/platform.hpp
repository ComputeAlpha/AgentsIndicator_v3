// OS glue: clock, paths, process inspection. Linux + macOS.
#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace plat {

// Milliseconds on a clock that never jumps with wall-clock changes (NTP, DST,
// manual) but does keep counting through suspend, so "waiting 3h" stays true
// after a laptop sleeps. Linux: CLOCK_BOOTTIME, macOS: CLOCK_MONOTONIC.
int64_t mono_ms();
// Unix epoch milliseconds (only for persistence across daemon restarts).
int64_t wall_ms();
// Local calendar day as YYYYMMDD (metric "today" boundaries).
int local_day();

std::string home_dir();
std::string config_dir();   // ~/.config/agent-indicator
std::string state_dir();    // ~/.local/state/agent-indicator
std::string socket_path();  // <state_dir>/daemon.sock
std::string self_exe();     // absolute path of the running binary
bool mkdirs(const std::string& path);
bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);

// Process helpers.
struct ProcInfo {
    pid_t ppid = 0;
    std::string name;        // short command name
    uint64_t start_id = 0;   // start time token; differs if the PID is reused
};
bool proc_info(pid_t pid, ProcInfo& out);
// True while `pid` exists and still has the same start token.
bool proc_alive(pid_t pid, uint64_t start_id);
// From the hook's parent, skip intermediate shells and return the Claude
// Code process (the hook's nearest non-shell ancestor).
pid_t find_session_process(pid_t start);

}  // namespace plat
