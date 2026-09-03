#pragma once
/**
 * wifi_control.hpp -- drives wpa_supplicant (via wpa_cli) and dhcpcd to
 * scan for networks and join one, without ever putting untrusted SSID/
 * passphrase bytes through a shell.
 *
 * SSID and PSK are both handed to wpa_cli as plain hex, and the PSK is
 * pre-derived ourselves with PBKDF2-HMAC-SHA1 (the same derivation
 * wpa_supplicant would do internally from a quoted passphrase) rather
 * than passed as a quoted ASCII string. Two reasons:
 *   1. wpa_supplicant's control-interface parser only strips a leading
 *      and trailing quote character -- it doesn't escape embedded quotes,
 *      so a passphrase containing '"' could desync the parser.
 *   2. Hex has no metacharacters at all, so there is nothing to escape
 *      and nothing for a hostile SSID/passphrase to break out of.
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <openssl/evp.h>

#include "subprocess.hpp"

struct ScanResult {
    std::string ssid;
    int rssi = 0;
    std::string security;
};

struct WifiStatus {
    enum State { IDLE, SCANNING, CONNECTING, CONNECTED, FAILED };
    State state = IDLE;
    std::string ssid;
    std::string ip;
    std::string error;

    const char* state_name() const {
        switch (state) {
            case IDLE:       return "idle";
            case SCANNING:   return "scanning";
            case CONNECTING: return "connecting";
            case CONNECTED:  return "connected";
            case FAILED:     return "failed";
        }
        return "idle";
    }
};

namespace wifi_detail {

inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

inline std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);
    return lines;
}

inline std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, delim)) parts.push_back(part);
    return parts;
}

inline std::string to_hex(const std::string& in) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

inline std::string bytes_to_hex(const unsigned char* buf, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[buf[i] >> 4]);
        out.push_back(digits[buf[i] & 0x0f]);
    }
    return out;
}

// WPA2-PSK derivation: PBKDF2-HMAC-SHA1(passphrase, ssid, 4096 iters, 256 bits).
inline std::string derive_psk_hex(const std::string& ssid, const std::string& passphrase) {
    unsigned char key[32];
    PKCS5_PBKDF2_HMAC(passphrase.c_str(), static_cast<int>(passphrase.size()),
                       reinterpret_cast<const unsigned char*>(ssid.data()), static_cast<int>(ssid.size()),
                       4096, EVP_sha1(), sizeof(key), key);
    return bytes_to_hex(key, sizeof(key));
}

// One line of `wpa_cli status` output, e.g. "wpa_state=COMPLETED".
inline std::string status_field(const std::string& status_output, const std::string& key) {
    for (auto& line : split_lines(status_output)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return "";
}

inline std::string security_from_flags(const std::string& flags) {
    if (flags.find("WPA2") != std::string::npos) return "WPA2";
    if (flags.find("WPA") != std::string::npos)  return "WPA";
    if (flags.find("WEP") != std::string::npos)  return "WEP";
    return "Open";
}

} // namespace wifi_detail

class WifiControl {
public:
    explicit WifiControl(std::string iface) : iface_(std::move(iface)) {}

    // Lazily re-checks wpa_supplicant's actual live state whenever we
    // haven't ourselves tracked a definite one yet in this process (i.e.
    // still at the IDLE default). A one-shot check at startup isn't
    // enough: wpa_supplicant's own service can be running before it has
    // actually finished reconnecting, so a single check at the exact
    // moment this object is constructed can race it and cache a stale
    // "idle" forever after, well before a BLE client ever gets around to
    // reading Status. Checking lazily on every read instead means it
    // keeps re-trying until either wpa_supplicant catches up or this
    // process performs its own connect()/forget(), which then takes
    // precedence over -- and stops -- the live re-check entirely, so it
    // never overwrites an in-progress CONNECTING/FAILED state we set
    // ourselves.
    WifiStatus get_status() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (status_.state != WifiStatus::IDLE) return status_;
        }
        refresh_status();
        std::lock_guard<std::mutex> lk(mu_);
        return status_;
    }

    void set_status(WifiStatus s) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = std::move(s);
    }

    void refresh_status() {
        using namespace wifi_detail;
        auto st = run_command({"wpa_cli", "-i", iface_, "status"});
        if (status_field(st.output, "wpa_state") == "COMPLETED") {
            std::string ssid = status_field(st.output, "ssid");
            // Not status_field(..., "ip_address") -- wpa_cli only reports
            // that field when wpa_supplicant itself manages DHCP (its own
            // -D dhcp integration or an action script), which this setup
            // doesn't use; dhcpcd runs as a separate client here, same as
            // connect() above, so the address has to come from the
            // interface itself the same way connect() reads it. Without
            // this, a restart while already connected (e.g. `rc-service
            // pi-bluetooth-configuration restart`) would see ssid but an
            // always-empty ip, fail this check, and get stuck reporting
            // IDLE forever -- the client would then show its "scanning
            // for networks" wizard step instead of the real connected
            // status, even though the Pi's WiFi never actually dropped.
            std::string ip = read_ipv4_address();
            if (!ssid.empty() && !ip.empty()) {
                set_status(WifiStatus{WifiStatus::CONNECTED, ssid, ip, ""});
            }
        }
    }

    // Blocking; run this on a worker thread. Triggers a scan and waits a
    // fixed settle time rather than tailing wpa_supplicant's event stream
    // -- simpler, and 4s is comfortably longer than a single-channel-list
    // active scan takes in practice.
    std::vector<ScanResult> scan(int max_results) {
        using namespace wifi_detail;
        run_command({"wpa_cli", "-i", iface_, "scan"});
        std::this_thread::sleep_for(std::chrono::seconds(4));

        auto res = run_command({"wpa_cli", "-i", iface_, "scan_results"});
        std::vector<ScanResult> out;
        auto lines = split_lines(res.output);
        for (size_t i = 1; i < lines.size(); ++i) { // skip header row
            auto cols = split(lines[i], '\t');
            if (cols.size() < 5) continue;
            const std::string& signal = cols[2];
            const std::string& flags = cols[3];
            const std::string& ssid = cols[4];
            if (ssid.empty()) continue; // hidden network, nothing to show

            ScanResult r;
            r.ssid = ssid;
            try { r.rssi = std::stoi(signal); } catch (...) { r.rssi = -100; }
            r.security = security_from_flags(flags);

            auto existing = std::find_if(out.begin(), out.end(),
                [&](const ScanResult& e) { return e.ssid == r.ssid; });
            if (existing == out.end()) out.push_back(r);
            else if (r.rssi > existing->rssi) *existing = r;
        }

        std::sort(out.begin(), out.end(), [](const ScanResult& a, const ScanResult& b) {
            return a.rssi > b.rssi;
        });
        if (static_cast<int>(out.size()) > max_results) out.resize(max_results);
        return out;
    }

    // Blocking; run this on a worker thread. Replaces every network
    // wpa_supplicant currently knows about (single active-network model)
    // and updates get_status() as it progresses.
    bool connect(const std::string& ssid, const std::string& psk) {
        using namespace wifi_detail;

        remove_all_networks();

        auto add = run_command({"wpa_cli", "-i", iface_, "add_network"});
        std::string id_str = trim(add.output);
        // Some wpa_cli builds print a header line before the result.
        for (auto& line : split_lines(add.output)) {
            std::string t = trim(line);
            if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos) { id_str = t; break; }
        }
        int id;
        try { id = std::stoi(id_str); }
        catch (...) { fail("could not parse network id from wpa_cli: " + add.output); return false; }

        auto set = [&](const std::string& field, const std::string& value) -> bool {
            auto r = run_command({"wpa_cli", "-i", iface_, "set_network", std::to_string(id), field, value});
            if (r.output.find("OK") == std::string::npos) {
                fail("wpa_cli set_network " + field + " failed: " + r.output);
                return false;
            }
            return true;
        };

        if (!set("ssid", to_hex(ssid))) return false;

        if (!psk.empty()) {
            if (!set("psk", derive_psk_hex(ssid, psk))) return false;
            if (!set("key_mgmt", "WPA-PSK")) return false;
        } else {
            if (!set("key_mgmt", "NONE")) return false;
        }

        run_command({"wpa_cli", "-i", iface_, "enable_network", std::to_string(id)});
        run_command({"wpa_cli", "-i", iface_, "select_network", std::to_string(id)});

        auto save = run_command({"wpa_cli", "-i", iface_, "save_config"});
        if (save.output.find("OK") == std::string::npos) {
            // Not fatal to this connection attempt -- association can
            // still succeed -- but this network will NOT survive a
            // reboot. wpa_cli returns FAIL for this when update_config=1
            // is missing from wpa_supplicant.conf, or when the config
            // file/directory isn't writable by the user running
            // wpa_supplicant.
            std::cerr << "[WifiControl] WARNING: wpa_cli save_config did not return OK (" << trim(save.output)
                      << ") -- this network will not persist across a reboot. Check that "
                         "update_config=1 is set in wpa_supplicant.conf and that the file is writable.\n";
        }

        set_status(WifiStatus{WifiStatus::CONNECTING, ssid, "", ""});

        const int poll_attempts = 20; // ~10s
        bool associated = false;
        for (int i = 0; i < poll_attempts; ++i) {
            auto st = run_command({"wpa_cli", "-i", iface_, "status"});
            if (status_field(st.output, "wpa_state") == "COMPLETED") { associated = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!associated) {
            fail("association timed out (check passphrase / signal)");
            return false;
        }

        auto lease = run_command({"dhcpcd", "-q", "-t", "15", iface_}, 20);
        if (lease.timed_out) {
            fail("dhcpcd timed out waiting for a lease");
            return false;
        }

        std::string ip = read_ipv4_address();
        if (ip.empty()) {
            fail("associated but no IPv4 address was assigned");
            return false;
        }

        set_status(WifiStatus{WifiStatus::CONNECTED, ssid, ip, ""});
        return true;
    }

    void forget() {
        remove_all_networks();
        run_command({"wpa_cli", "-i", iface_, "save_config"});
        set_status(WifiStatus{});
    }

private:
    void fail(const std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = WifiStatus::FAILED;
        status_.error = err;
    }

    // wpa_supplicant reloads every network saved in wpa_supplicant.conf
    // on each of its own restarts (which happens on every reboot this
    // daemon triggers after a successful connect/forget), so an
    // in-process "last id I added" can't track what's actually
    // configured -- it only knows about networks added since this
    // daemon process itself started. Querying the live list instead
    // means "remove everything" is correct regardless of how
    // wpa_supplicant got to its current state.
    void remove_all_networks() {
        auto list = run_command({"wpa_cli", "-i", iface_, "list_networks"});
        for (auto& line : wifi_detail::split_lines(list.output)) {
            auto cols = wifi_detail::split(line, '\t');
            if (cols.empty()) continue;
            std::string id_field = wifi_detail::trim(cols[0]);
            if (!id_field.empty() && id_field.find_first_not_of("0123456789") == std::string::npos) {
                run_command({"wpa_cli", "-i", iface_, "remove_network", id_field});
            }
        }
    }

    std::string read_ipv4_address() {
        auto r = run_command({"ip", "-4", "-o", "addr", "show", "dev", iface_});
        auto pos = r.output.find(" inet ");
        if (pos == std::string::npos) return "";
        pos += 6;
        auto slash = r.output.find('/', pos);
        if (slash == std::string::npos) return "";
        return wifi_detail::trim(r.output.substr(pos, slash - pos));
    }

    std::string iface_;
    mutable std::mutex mu_;
    WifiStatus status_;
};
