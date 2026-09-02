#pragma once
/**
 * victron_control.hpp -- TCP client for victron-ve-direct-alpine's status
 * control port: sends "status", parses the plain "key=value" reply (one
 * connection per query, same one-shot connect/send/recv/close pattern as
 * pi-relay-control-alpine's relay ports -- see that project's README),
 * and re-shapes it into JSON using the same field names as
 * victron-ve-direct-alpine's own data_port telemetry frames, so a client
 * doesn't need to learn two different naming schemes for the same data.
 *
 * This is a soft, optional integration, same as relay_control.hpp:
 * victron-ve-direct-alpine is a separate package this daemon doesn't
 * require, start, or own. If it isn't installed, isn't running, isn't
 * connected to an actual VE.Direct device yet, or its ctrl_port doesn't
 * match config.ini's "[victron]" section, a query here just fails or
 * comes back "ok=false" and reports {"connected":false} -- it never
 * blocks or crashes the BLE service over it.
 */
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace victronctl {

// SOC/TTG (state of charge, time to go) exist on the wire for battery
// monitors (BMV-series) but are deliberately not modeled here -- this
// integration targets MPPT solar chargers, which don't report them.
// LOAD (the charger's load output switch state) is also deliberately
// not modeled here -- not every MPPT model has a load output terminal,
// and on this integration's hardware it's always "ON", so there's
// nothing informative to show.
struct VictronStatus {
    bool connected = false;
    std::string device_name, pid, serial, fw;
    double V = 0, I = 0, VPV = 0, PPV = 0, H20 = 0;
    int CS = 0, ERR = 0;
    std::string CS_name, ERR_name;
};

// Sends one command to victron-ve-direct-alpine's ctrl_port on
// 127.0.0.1 and returns its raw multi-line reply. Unlike
// relay_control.hpp's single fixed-size recv (relay replies are one
// short line), this loops until the peer closes the connection, since a
// "status" reply is several lines and can exceed one recv's worth.
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

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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

    std::string resp;
    char buf[512];
    while (true) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break; // peer closed (expected, one reply per connection) or timed out
        resp.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    if (resp.empty()) err = "no response from victron-ve-direct on port " + std::to_string(port);
    return resp;
}

namespace detail {

inline std::unordered_map<std::string, std::string> parse_kv(const std::string& text) {
    std::unordered_map<std::string, std::string> kv;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

inline std::string str(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    return it == kv.end() ? "" : it->second;
}

inline double num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) return 0;
    try { return std::stod(it->second); } catch (...) { return 0; }
}

inline int inum(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    return static_cast<int>(num(kv, key));
}

} // namespace detail

// Queries the latest telemetry snapshot. Returns a VictronStatus with
// connected=false (all other fields default) if the daemon isn't
// reachable, or replied anything other than "ok=true" (e.g. it's up but
// hasn't synced a frame from the VE.Direct device yet).
inline VictronStatus query_status(int port) {
    VictronStatus s;
    std::string err;
    std::string resp = send_command(port, "status", err);
    if (resp.empty()) return s;

    auto kv = detail::parse_kv(resp);
    if (detail::str(kv, "ok") != "true") return s;

    s.connected = true;
    s.device_name = detail::str(kv, "device");
    s.pid = detail::str(kv, "pid");
    s.serial = detail::str(kv, "serial");
    s.fw = detail::str(kv, "fw");
    s.V = detail::num(kv, "V");
    s.I = detail::num(kv, "I");
    s.VPV = detail::num(kv, "VPV");
    s.PPV = detail::num(kv, "PPV");
    s.CS = detail::inum(kv, "CS");
    s.CS_name = detail::str(kv, "CS_name");
    s.ERR = detail::inum(kv, "ERR");
    s.ERR_name = detail::str(kv, "ERR_name");
    s.H20 = detail::num(kv, "H20");
    return s;
}

} // namespace victronctl
