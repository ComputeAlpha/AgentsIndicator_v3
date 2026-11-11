// Session feed shared between daemons on different machines.
//
// A daemon broadcasts its *local* sessions once per second (and on change) to
// every client connected to 127.0.0.1:<broadcast_port>. A daemon with the
// indicator attached connects to one or more feed ports (normally the local
// end of an SSH forward, `ssh -L 47292:localhost:47291 host`) and merges what
// it receives into its own display. Feeds are never re-broadcast, so chains
// and loops can't double count.
//
// Wire format: one compact JSON object per line.
//   {"v":1,"host":"name","s":[["W","",in_state_ms,streak_ms],...],
//    "d":[agent_ms,busy_ms,wait_ms,waits,idle_ms,idles,tools,started],
//    "apm":actions_per_min*100,"ia":inactive_ms}
// state: W working, Y waiting, I idle, D done, E error; wait: "p" permission,
// "t" finished turn. Only states and counters go out: no paths, prompts or ids.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "composer.hpp"

namespace feed {

constexpr int kVersion = 1;

std::string encode(const View& v, const std::string& host);
bool decode(const std::string& line, View& out, std::string& host);

// Listens on 127.0.0.1:port and writes each published line to every client.
class Server {
public:
    ~Server() { close_all(); }
    bool listen(int port);            // false (errno set) if the port can't be bound
    bool listening() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void accept_pending();
    void publish(const std::string& line);   // line without '\n'
    size_t clients() const { return clients_.size(); }
    void close_all();

private:
    int fd_ = -1;
    std::vector<int> clients_;
};

// One remote feed, reached through a local TCP port (an SSH forward).
class Remote {
public:
    explicit Remote(std::string target);   // "port" or "host:port"
    ~Remote() { disconnect(); }
    Remote(const Remote&) = delete;
    Remote& operator=(const Remote&) = delete;

    // Connects when due (with back-off), reads lines, expires stale data.
    // Call with fd() readable or periodically; returns true if the view changed.
    bool poll(int64_t now);
    int fd() const { return fd_; }
    // The remote's sessions, aged to `now` (nothing if not live).
    bool live(int64_t now) const;
    View view(int64_t now) const;
    const std::string& target() const { return target_; }
    const std::string& host() const { return host_; }
    // State-change messages for the log ("connected", "lost" ...).
    std::vector<std::string> take_log() { return std::move(log_); }

private:
    void disconnect();
    void fail(int64_t now, const char* why);

    std::string target_, addr_ = "127.0.0.1", host_;
    int port_ = 0;
    int fd_ = -1;
    bool connecting_ = false;
    std::string buf_;
    View last_;
    int64_t last_rx_ = -1;          // receipt time of last_
    int64_t next_try_ = 0;
    int64_t backoff_ms_ = 3000;
    bool announced_ = false;        // logged "receiving"
    std::vector<std::string> log_;
};

}  // namespace feed
