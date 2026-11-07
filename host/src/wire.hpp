// Datagram format between `agent-indicator hook|raw` and the daemon.
//   EV1 \t name \t session \t agent_id \t detail \t tool \t pid \t cwd
//   CTL1 \t raw \t <device line>
#pragma once
#include <sstream>
#include <string>
#include <sys/types.h>
#include <vector>

namespace wire {

struct Event {
    std::string name, session, agent_id, detail, tool, cwd;
    pid_t pid = 0;
};

inline std::string clean(const std::string& s, size_t cap = 512) {
    std::string o;
    o.reserve(std::min(s.size(), cap));
    for (char c : s) {
        if (o.size() >= cap) break;
        o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    return o;
}

inline std::vector<std::string> split_tabs(const std::string& s) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : s) {
        if (c == '\t') { f.push_back(cur); cur.clear(); }
        else cur += c;
    }
    f.push_back(cur);
    return f;
}

inline std::string encode(const Event& e) {
    return "EV1\t" + clean(e.name, 64) + "\t" + clean(e.session, 128) + "\t" + clean(e.agent_id, 128) + "\t" +
           clean(e.detail, 64) + "\t" + clean(e.tool, 128) + "\t" + std::to_string(e.pid) + "\t" + clean(e.cwd, 1024);
}

inline bool decode(const std::string& msg, Event& e) {
    auto f = split_tabs(msg);
    if (f.size() != 8 || f[0] != "EV1") return false;
    e.name = f[1];
    e.session = f[2];
    e.agent_id = f[3];
    e.detail = f[4];
    e.tool = f[5];
    e.pid = (pid_t)atol(f[6].c_str());
    e.cwd = f[7];
    return true;
}

}  // namespace wire
