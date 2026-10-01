#pragma once
/**
 * relay_control.hpp -- TCP client for pi-relay-control-alpine's per-relay
 * socket protocol ("on" / "off" / "status" -> "OK RELAY=ON" / "OK
 * RELAY=OFF" / "RELAY=ON"|"RELAY=OFF"), so this daemon can trigger relays
 * that pi-relay-control-alpine manages on the same Pi without a shell or
 * an `nc` subprocess -- plain sockets, matching the one-shot
 * connect/send/recv/close pattern that server itself expects (see
 * pi-relay-control-alpine's README/src/main.cpp).
 *
 * This is a soft, optional integration: pi-relay-control-alpine is a
 * separate package this daemon doesn't require, start, or own. If it
 * isn't installed, isn't running, or its config lists different ports
 * than the "[relays]" section below, every call here just fails to
 * connect and reports state "unknown" -- it never blocks or crashes the
 * HTTP server over it.
 *
 * relay_always_on()/set_relay_always_on() below go further than the TCP
 * protocol: "always_on" (force a relay ON at pi-relay-control-alpine's
 * own startup, ignoring whatever state was last persisted) has no live
 * TCP command at all -- it's read once, at that daemon's own startup,
 * from a "relay <gpio_pin> <port> [always_on]" line in its config file
 * (see pi-relay-control-alpine's own README/src/main.cpp). Toggling it
 * from here means editing that sibling package's config file directly
 * (matching by <port>, the one field both daemons' configs share) and
 * restarting it so the new value actually takes effect -- the same
 * "reach into a config file this daemon doesn't own" pattern
 * eth_control.hpp already uses for dhcpcd.conf/dnsmasq.conf, just for a
 * single existing key/line instead of a whole marker-delimited block.
 */
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace relayctl {

struct RelayConfig {
    int port;
    std::string label;
};

// One relay per "relay <port> <label>" line under a "[relays]" section
// of config.ini -- a small dedicated scan rather than the Config class,
// since Config only keeps one value per key and this needs a repeatable
// list. Mirrors how pi-relay-control-alpine itself parses its own
// "relay <gpio_pin> <port> [always_on]" lines rather than shoehorning a
// list into a single-value INI key. <port> here must match the TCP port
// assigned to that same relay in pi-relay-control-alpine's own
// /etc/pi-relay-control.conf -- this file only carries the port and a
// display label, not the GPIO pin, since that's pi-relay-control-alpine's
// concern, not this daemon's.
//
// Always returns every "relay" line regardless of the "enabled" flag
// (see relays_enabled() below) -- main.cpp keeps this list fixed for the
// life of the process (the physical relays/ports it knows about don't
// change without an edit + restart) and layers the enabled/disabled
// runtime toggle on top via a separate std::atomic<bool>, so a web UI
// toggle can flip that without re-parsing or restarting anything.
inline std::vector<RelayConfig> load_relays(const std::string& config_path) {
    std::vector<RelayConfig> relays;
    std::ifstream file(config_path);
    if (!file.is_open()) return relays;

    std::string line;
    bool in_relays_section = false;
    while (std::getline(file, line)) {
        auto comment = line.find_first_of(";#");
        if (comment != std::string::npos) line = line.substr(0, comment);

        auto not_ws = line.find_first_not_of(" \t\r\n");
        if (not_ws == std::string::npos) continue;
        std::string trimmed = line.substr(not_ws);

        if (trimmed.front() == '[') {
            in_relays_section = (trimmed.rfind("[relays]", 0) == 0);
            continue;
        }
        if (!in_relays_section) continue;

        std::istringstream iss(trimmed);
        std::string key;
        if (!(iss >> key) || key != "relay") continue;

        RelayConfig r;
        if (!(iss >> r.port)) {
            std::cerr << "[Relay] malformed relay config line, expected: relay <port> <label>\n";
            continue;
        }
        std::string word;
        while (iss >> word) {
            if (!r.label.empty()) r.label += " ";
            r.label += word;
        }
        if (r.label.empty()) r.label = "Relay " + std::to_string(r.port);
        relays.push_back(r);
    }
    return relays;
}

// Reads the "enabled" key from "[relays]" -- true (relay control on) if
// absent, matching this feature's original all-or-nothing-via-the-list
// behavior from before this toggle existed. Read once at startup to
// seed main.cpp's own std::atomic<bool> (see load_relays()'s own
// comment) -- not called again afterward, since set_relays_enabled()
// below keeps that atomic and the on-disk value in sync itself.
inline bool relays_enabled(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) return true;

    std::string line;
    bool in_relays_section = false;
    while (std::getline(file, line)) {
        auto comment = line.find_first_of(";#");
        if (comment != std::string::npos) line = line.substr(0, comment);

        auto not_ws = line.find_first_not_of(" \t\r\n");
        if (not_ws == std::string::npos) continue;
        std::string trimmed = line.substr(not_ws);

        if (trimmed.front() == '[') {
            in_relays_section = (trimmed.rfind("[relays]", 0) == 0);
            continue;
        }
        if (!in_relays_section) continue;

        std::istringstream iss(trimmed);
        std::string key;
        if (!(iss >> key) || key != "enabled") continue;
        std::string value;
        iss >> value;
        return !(value == "false" || value == "0" || value == "no");
    }
    return true;
}

// Rewrites (or inserts, if it wasn't already there) the "enabled" line
// in "[relays]" so a web UI toggle survives a reboot, not just this
// running process -- same "the file is the source of truth, the daemon
// just edits it in place" approach eth_control.hpp's replace_marker_block
// uses, but here editing a real key inside an existing user-owned
// section (relay lines, comments, ordering) rather than a whole
// marker-delimited block, since [relays] isn't a section this daemon
// owns exclusively the way its own eth0/AP config blocks are.
inline bool set_relays_enabled(const std::string& config_path, bool enabled, std::string& err) {
    std::ifstream in(config_path);
    if (!in.is_open()) {
        err = "could not open " + config_path;
        return false;
    }
    std::ostringstream out;
    std::string line;
    bool in_relays_section = false;
    bool wrote_enabled = false;
    while (std::getline(in, line)) {
        auto not_ws = line.find_first_not_of(" \t\r\n");
        std::string trimmed = (not_ws == std::string::npos) ? "" : line.substr(not_ws);

        if (!trimmed.empty() && trimmed.front() == '[') {
            // Leaving [relays] without ever finding its own "enabled"
            // line -- insert one now, right at the top of the section,
            // before falling through to whatever section starts here.
            if (in_relays_section && !wrote_enabled) {
                out << "enabled = " << (enabled ? "true" : "false") << "\n";
                wrote_enabled = true;
            }
            in_relays_section = (trimmed.rfind("[relays]", 0) == 0);
            out << line << "\n";
            continue;
        }

        if (in_relays_section) {
            auto comment = trimmed.find_first_of(";#");
            std::string code = (comment == std::string::npos) ? trimmed : trimmed.substr(0, comment);
            std::istringstream iss(code);
            std::string key;
            if ((iss >> key) && key == "enabled") {
                out << "enabled = " << (enabled ? "true" : "false") << "\n";
                wrote_enabled = true;
                continue;
            }
        }
        out << line << "\n";
    }
    // The whole file was scanned still "inside" [relays] (it's the last
    // section, or the only one) and never had its own "enabled" line --
    // append it at the very end of the section/file.
    if (in_relays_section && !wrote_enabled) {
        out << "enabled = " << (enabled ? "true" : "false") << "\n";
    }
    in.close();

    std::ofstream file_out(config_path, std::ios::trunc);
    if (!file_out.is_open()) {
        err = "could not write " + config_path;
        return false;
    }
    file_out << out.str();
    return true;
}

// Sends one command ("on" | "off" | "status") to the relay listening on
// 127.0.0.1:<port>, and returns its raw response with the trailing
// newline stripped. An empty return with `err` set means the connection
// itself failed (daemon not running, wrong port, etc.) -- distinct from
// the daemon replying with "ERR ...", which is returned as-is for the
// caller to interpret.
inline std::string send_command(int port, const std::string& cmd, std::string& err) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket() failed: ") + strerror(errno);
        return "";
    }

    timeval tv{2, 0}; // 2s -- loopback, should be near-instant if the daemon is up
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // connect() itself isn't covered by SO_*TIMEO above (those only bound
    // send/recv) -- on loopback it's normally instant, but this daemon
    // holds a per-port mutex (see main.cpp's relay_port_mu) for the
    // duration of this call, so an unbounded connect() to one stuck or
    // unresponsive relay could hang every future command/query for that
    // same port indefinitely rather than just failing this one call.
    // Non-blocking connect + select() bounds it to the same 2s budget.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval connect_tv{2, 0};
        rc = select(fd + 1, nullptr, &wfds, nullptr, &connect_tv);
        if (rc > 0) {
            int so_err = 0;
            socklen_t so_err_len = sizeof(so_err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len);
            rc = (so_err == 0) ? 0 : -1;
            if (so_err != 0) errno = so_err;
        } else if (rc == 0) {
            errno = ETIMEDOUT;
            rc = -1;
        }
    }
    fcntl(fd, F_SETFL, flags); // restore blocking mode for send()/recv() below
    if (rc < 0) {
        err = "connect to 127.0.0.1:" + std::to_string(port) + " failed: " + strerror(errno);
        close(fd);
        return "";
    }

    std::string line = cmd + "\n";
    if (send(fd, line.c_str(), line.size(), 0) < 0) {
        err = std::string("send() failed: ") + strerror(errno);
        close(fd);
        return "";
    }

    char buf[256] = {};
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) {
        err = "no response from relay on port " + std::to_string(port);
        return "";
    }

    std::string resp(buf, static_cast<size_t>(n));
    while (!resp.empty() && (resp.back() == '\n' || resp.back() == '\r')) resp.pop_back();
    return resp;
}

// Queries live state via "status" -- "on" | "off" | "unknown". "unknown"
// covers both a connection failure and any reply that isn't the
// well-known "RELAY=ON"/"RELAY=OFF" format, so a client always gets a
// value to display rather than an error it has to special-case.
inline std::string query_state(int port) {
    std::string err;
    std::string resp = send_command(port, "status", err);
    if (resp == "RELAY=ON") return "on";
    if (resp == "RELAY=OFF") return "off";
    return "unknown";
}

// pi-relay-control-alpine's own config file, not this daemon's --
// see this file's own header comment for why always_on has to be
// edited there directly rather than over the TCP protocol.
constexpr const char* PI_RELAY_CONTROL_CONF = "/etc/pi-relay-control.conf";

// Finds the "relay <gpio_pin> <port> [always_on]" line whose <port>
// matches, using the exact same comment-stripping (bare "#", anywhere
// on the line) and whitespace-splitting rules as pi-relay-control-alpine's
// own loadConfig(), so this never disagrees with what that daemon itself
// will parse. Returns false (not just "false state") if the file can't
// be read or no line configures this port at all -- distinct from "line
// found, no always_on token," which is a real, valid false.
inline bool relay_always_on(int port) {
    std::ifstream file(PI_RELAY_CONTROL_CONF);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key) || key != "relay") continue;

        int gpio_pin, line_port;
        if (!(iss >> gpio_pin >> line_port)) continue;
        if (line_port != port) continue;

        std::string opt;
        while (iss >> opt) {
            if (opt == "always_on") return true;
        }
        return false;
    }
    return false;
}

// Rewrites the matching "relay <gpio_pin> <port> [always_on]" line in
// place, adding or removing the trailing "always_on" token -- every
// other line (comments, other relays, blank lines) is copied through
// byte-for-byte. Fails with an explanatory err (rather than silently
// doing nothing) if PI_RELAY_CONTROL_CONF can't be read/written, or if
// no "relay" line configures this port at all -- the latter means
// pi-bluetooth-configuration's own config.ini's "[relays]" and
// pi-relay-control-alpine's own config have drifted out of sync (see
// this daemon's own README, "Relay control", on why <port> must match
// between the two), which the caller should surface, not paper over.
inline bool set_relay_always_on(int port, bool always_on, std::string& err) {
    std::ifstream in(PI_RELAY_CONTROL_CONF);
    if (!in.is_open()) {
        err = "could not open " + std::string(PI_RELAY_CONTROL_CONF);
        return false;
    }
    std::ostringstream out;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        auto comment = line.find('#');
        std::string code = (comment == std::string::npos) ? line : line.substr(0, comment);
        std::string trailing_comment = (comment == std::string::npos) ? "" : line.substr(comment);

        std::istringstream iss(code);
        std::string key;
        int gpio_pin, line_port;
        bool matches = (iss >> key) && key == "relay" && (iss >> gpio_pin >> line_port) && line_port == port;
        if (!matches) {
            out << line << "\n";
            continue;
        }

        found = true;
        out << "relay " << gpio_pin << " " << line_port;
        if (always_on) out << " always_on";
        if (!trailing_comment.empty()) out << " " << trailing_comment;
        out << "\n";
    }
    in.close();

    if (!found) {
        err = "no \"relay\" line for port " + std::to_string(port) + " in " +
              std::string(PI_RELAY_CONTROL_CONF);
        return false;
    }

    std::ofstream file_out(PI_RELAY_CONTROL_CONF, std::ios::trunc);
    if (!file_out.is_open()) {
        err = "could not write " + std::string(PI_RELAY_CONTROL_CONF);
        return false;
    }
    file_out << out.str();
    return true;
}

} // namespace relayctl
