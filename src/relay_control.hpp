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
 * BLE service over it.
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

} // namespace relayctl
