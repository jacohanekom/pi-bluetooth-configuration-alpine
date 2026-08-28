#pragma once
/**
 * tcp_control.hpp -- plain JSON-over-TCP control interface, always
 * listening once the daemon starts.
 *
 * Why this exists: the Pi 3's onboard Bluetooth chip (BCM43438) shares a
 * single antenna/RF front-end with its WiFi radio. Once WiFi is actively
 * associated and passing traffic, it routinely starves the BLE
 * connection and drops it -- a hardware limitation, not a bug in this
 * daemon or in any particular BLE central. So: BLE is only relied on for
 * the one thing it has to do (get credentials across before any network
 * exists). Once the Status characteristic reports a non-empty ip, a
 * well-behaved client hands off to this TCP interface instead, which
 * isn't subject to that coexistence problem at all.
 *
 * Protocol: one JSON object per line, in each direction; the connection
 * stays open across multiple commands.
 *   -> {"cmd":"status"}                          <- {"state":...,"ssid":...,"ip":...,"error":...}
 *   -> {"cmd":"scan"}                             <- {"ok":true}
 *   -> {"cmd":"scanresults"}                      <- [{"ssid":...,"rssi":...,"security":...}, ...]
 *   -> {"cmd":"connect","ssid":"...","psk":"..."} <- {"ok":true}
 *   -> {"cmd":"forget"}                           <- {"ok":true}
 *
 * "scan" and "connect" reply immediately with an ack and do the actual
 * work on a separate thread, exactly like the BLE Command characteristic
 * -- poll "status"/"scanresults" separately rather than expecting the
 * ack to carry the outcome.
 *
 * NOTE: unauthenticated, like this project family's other TCP control
 * ports (mp3-player, victron-ve-direct) -- anyone who can reach this
 * port on the LAN can read status or reconfigure WiFi credentials.
 * Acceptable for a single-purpose home/lab Pi; not for a shared or
 * untrusted network. See the README.
 */
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcpctl {

// Minimal extraction of one string field from a single-level JSON object
// -- this protocol never nests, so a full parser would be pure overhead.
inline std::string extract_string_field(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            out.push_back(json[pos + 1]);
            pos += 2;
        } else {
            out.push_back(json[pos]);
            ++pos;
        }
    }
    return out;
}

class TcpControlServer {
public:
    std::function<std::string()> on_status;
    std::function<std::string()> on_scan_results;
    std::function<void()> on_scan;
    std::function<void(std::string ssid, std::string psk)> on_connect;
    std::function<void()> on_forget;

    void start(int port) {
        std::thread(&TcpControlServer::run, this, port).detach();
    }

private:
    void run(int port) {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[TCP] Failed to bind port " << port << ": " << strerror(errno) << "\n";
            return;
        }
        listen(server_fd, 4);
        std::cerr << "[TCP] Control interface listening on 0.0.0.0:" << port << "\n";

        while (true) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;
            std::thread(&TcpControlServer::handle_client, this, client_fd).detach();
        }
    }

    void handle_client(int fd) {
        std::string buf;
        char chunk[4096];
        while (true) {
            ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));

            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty()) continue;
                std::string reply = handle_command(line) + "\n";
                write(fd, reply.data(), reply.size());
            }
        }
        close(fd);
    }

    std::string handle_command(const std::string& line) {
        std::string cmd = extract_string_field(line, "cmd");

        if (cmd == "status") {
            return on_status ? on_status() : "{}";
        }
        if (cmd == "scan") {
            if (on_scan) on_scan();
            return "{\"ok\":true}";
        }
        if (cmd == "scanresults") {
            return on_scan_results ? on_scan_results() : "[]";
        }
        if (cmd == "connect") {
            if (on_connect) on_connect(extract_string_field(line, "ssid"), extract_string_field(line, "psk"));
            return "{\"ok\":true}";
        }
        if (cmd == "forget") {
            if (on_forget) on_forget();
            return "{\"ok\":true}";
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }
};

} // namespace tcpctl
