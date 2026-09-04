#pragma once
/**
 * http_server.hpp -- minimal, dependency-free HTTP/1.1 server replacing
 * this daemon's old BLE GATT peripheral role entirely. See main.cpp's
 * header comment and the README for the full story of why: BlueZ's own
 * built-in GATT profiles forced a disconnect loop this daemon had no
 * clean way to fix, and a Pico-hosted BLE bridge (briefly tried) was
 * itself scrapped in favor of this -- a plain WiFi AP + HTTP API is a
 * far more standard, battle-tested pattern for headless device setup
 * (the same shape ESP8266/ESP32 WiFiManager-style devices use), and
 * reuses infrastructure (hostapd, dnsmasq) this project already needed
 * for other things.
 *
 * Thread-per-connection: a slow request (e.g. one that ends up querying
 * pi-relay-control-alpine or victron-ve-direct-alpine, each with their
 * own multi-second timeout budget) only ever blocks its own connection,
 * never a shared dispatch loop the way a single BlueZ D-Bus thread did --
 * an entire earlier class of bug (see git history) simply doesn't exist
 * in this design.
 *
 * No server push: unlike BLE's notify, plain HTTP has no equivalent
 * without something heavier (SSE, WebSockets) this project doesn't need.
 * Clients are expected to poll GET /status periodically instead -- see
 * main.cpp's route registrations for exactly what that returns. This is
 * simpler and more robust than the old notify-plus-cache machinery: no
 * missed-notification or subscription-state bookkeeping anywhere.
 *
 * Deliberately not parsing anything beyond what this daemon's own
 * clients (this project's iOS/macOS apps, using plain URLSession) ever
 * send: no chunked transfer-encoding, no keep-alive (every response
 * closes the connection), no multipart. A minimal request line +
 * headers + optional Content-Length-delimited body is the entire
 * surface, matching the same "hand-roll only what's actually needed"
 * style already used throughout this project (relay_control.hpp,
 * victron_control.hpp, the old gatt_server.hpp).
 */
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace httpsrv {

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

struct Response {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";

    static Response json(const std::string& body, int status = 200) {
        return Response{status, body, "application/json"};
    }
    static Response error(int status, const std::string& message) {
        return Response{status, "{\"error\":\"" + message + "\"}", "application/json"};
    }
};

class HttpServer {
public:
    using Handler = std::function<Response(const Request&)>;

    void route(const std::string& method, const std::string& path, Handler h) {
        routes_[method + " " + path] = std::move(h);
    }

    // Binds 0.0.0.0:port so requests are served regardless of which
    // network is currently live (the AP's own subnet while unconfigured,
    // or whatever real network was joined afterward) without needing to
    // restart this server across that transition.
    bool start(int port, std::string& err_out) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            err_out = std::string("socket() failed: ") + strerror(errno);
            return false;
        }

        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            err_out = "bind(:" + std::to_string(port) + ") failed: " + strerror(errno);
            return false;
        }
        if (listen(listen_fd_, 16) < 0) {
            err_out = std::string("listen() failed: ") + strerror(errno);
            return false;
        }

        running_ = true;
        accept_thread_ = std::thread([this]() { accept_loop(); });
        return true;
    }

private:
    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }
            std::thread(&HttpServer::handle_connection, this, client_fd).detach();
        }
    }

    // Reads exactly one HTTP/1.1 request (request line + headers,
    // then Content-Length body bytes if present), dispatches it to
    // whichever handler matches "<METHOD> <path>" (query strings, if
    // any, are stripped before matching), and always closes the
    // connection afterward -- see this file's header comment on why
    // keep-alive isn't supported.
    void handle_connection(int client_fd) {
        std::string buf;
        char chunk[4096];
        size_t header_end = std::string::npos;

        while (header_end == std::string::npos) {
            ssize_t n = read(client_fd, chunk, sizeof(chunk));
            if (n <= 0) { close(client_fd); return; }
            buf.append(chunk, static_cast<size_t>(n));
            header_end = buf.find("\r\n\r\n");
            if (buf.size() > 65536 && header_end == std::string::npos) {
                send_raw(client_fd, Response::error(431, "request headers too large"));
                close(client_fd);
                return;
            }
        }

        std::istringstream head(buf.substr(0, header_end));
        std::string request_line;
        std::getline(head, request_line);
        if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

        std::istringstream rl(request_line);
        Request req;
        std::string http_version;
        rl >> req.method >> req.path >> http_version;

        auto qpos = req.path.find('?');
        if (qpos != std::string::npos) req.path.erase(qpos);

        size_t content_length = 0;
        std::string header_line;
        while (std::getline(head, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
            auto colon = header_line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = header_line.substr(0, colon);
            for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (name == "content-length") {
                try {
                    content_length = static_cast<size_t>(std::stoul(header_line.substr(colon + 1)));
                } catch (...) {
                    content_length = 0;
                }
            }
        }

        std::string body_so_far = buf.substr(header_end + 4);
        while (body_so_far.size() < content_length) {
            ssize_t n = read(client_fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            body_so_far.append(chunk, static_cast<size_t>(n));
        }
        req.body = body_so_far.substr(0, content_length);

        auto it = routes_.find(req.method + " " + req.path);
        Response resp = (it != routes_.end()) ? it->second(req) : Response::error(404, "not found");
        send_raw(client_fd, resp);
        close(client_fd);
    }

    void send_raw(int fd, const Response& resp) {
        static const std::map<int, std::string> reasons = {
            {200, "OK"}, {400, "Bad Request"}, {404, "Not Found"},
            {405, "Method Not Allowed"}, {431, "Request Header Fields Too Large"},
            {500, "Internal Server Error"},
        };
        auto it = reasons.find(resp.status);
        std::string reason = it != reasons.end() ? it->second : "";

        std::ostringstream out;
        out << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
            << "Content-Type: " << resp.content_type << "\r\n"
            << "Content-Length: " << resp.body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << resp.body;
        std::string s = out.str();
        ssize_t n = write(fd, s.data(), s.size());
        (void)n;
    }

    std::map<std::string, Handler> routes_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};

} // namespace httpsrv
