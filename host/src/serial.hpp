// USB CDC link to the indicator (POSIX termios; Linux + macOS).
#pragma once
#include <string>
#include <vector>

class SerialLink {
public:
    ~SerialLink() { close(); }

    // Opens `path` (or auto-detects when empty) and verifies the device
    // answers "?" with "AGENTIND v3". On success, `ident()` holds its reply.
    bool open(const std::string& path_or_empty);
    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    const std::string& ident() const { return ident_; }

    // Non-blocking line write. Returns false if the device went away.
    bool send_line(const std::string& line);
    // Drain and return any complete lines received (non-blocking).
    std::vector<std::string> read_lines();

    static std::vector<std::string> candidates();

private:
    bool open_one(const std::string& path);
    int fd_ = -1;
    std::string path_, ident_, rx_;
};
