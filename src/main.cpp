/**
 * pi-bluetooth-configuration
 * ===========================
 * BLE GATT peripheral that lets a phone/app join this Pi to a WiFi network
 * without SSH or a keyboard: pair over Bluetooth LE, write an SSID and
 * passphrase, write "connect", and watch the status characteristic for the
 * result. Built directly on BlueZ's D-Bus API (see gatt_server.hpp) --
 * scanning/joining/DHCP are driven through wpa_cli and dhcpcd (see
 * wifi_control.hpp).
 *
 * GATT service (see README for the exact UUIDs):
 *   SSID        (write)          stage a network name
 *   Password    (write)          stage a passphrase (omit for open networks)
 *   Command     (write)          "scan" | "connect" | "forget"
 *   Status      (read + notify)  {"state":...,"ssid":...,"ip":...,"error":...}
 *   ScanResults (read + notify)  [{"ssid":...,"rssi":...,"security":...}, ...]
 *
 * No pairing/bonding: characteristics are plain read/write, not
 * encrypted. An earlier revision required BLE pairing, but bonding on the
 * Pi 3's BCM43438 proved unreliable enough in practice (bond desync
 * between BlueZ and the central, repeated OS-level pairing prompts) that
 * it wasn't worth what it protected against -- see the README's Security
 * model section. This means WiFi credentials cross BLE in the clear;
 * treat this as suitable for a trusted home/lab network, not a public one.
 *
 * The Pi 3's onboard Bluetooth shares an antenna with its WiFi radio, so
 * BLE connections routinely drop once WiFi is actively passing traffic --
 * a hardware limitation, not a bug here. So BLE is only relied on to get
 * credentials across before any network exists; see tcp_control.hpp for
 * the plain TCP/IP interface a client should hand off to once Status
 * reports a non-empty ip.
 *
 * Build (Alpine Linux):
 *   make
 * Run:
 *   ./pi-bluetooth-configuration [--config config.ini]
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "config.hpp"
#include "gatt_server.hpp"
#include "tcp_control.hpp"
#include "wifi_control.hpp"

namespace {

constexpr const char* SERVICE_UUID  = "7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* SSID_UUID     = "7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* PSK_UUID      = "7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* COMMAND_UUID  = "7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* STATUS_UUID   = "7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* SCAN_UUID     = "7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* APP_ROOT      = "/org/bluez/pibtconf";

std::atomic<bool> g_running{true};
std::atomic<int> g_inflight{0};

void on_signal(int) { g_running = false; }

// RAII guard so a worker thread always decrements g_inflight, even if
// wifi.scan()/connect() throws -- lets shutdown wait for it to actually
// finish before main() destroys the objects it captured by reference.
struct InflightGuard {
    InflightGuard() { ++g_inflight; }
    ~InflightGuard() { --g_inflight; }
};

std::vector<uint8_t> to_bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string status_json(const WifiStatus& s) {
    std::ostringstream o;
    o << "{\"state\":\"" << s.state_name() << "\","
      << "\"ssid\":\"" << escape_json(s.ssid) << "\","
      << "\"ip\":\"" << escape_json(s.ip) << "\","
      << "\"error\":\"" << escape_json(s.error) << "\"}";
    return o.str();
}

std::string scan_json(const std::vector<ScanResult>& results) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i) o << ",";
        o << "{\"ssid\":\"" << escape_json(results[i].ssid) << "\","
          << "\"rssi\":" << results[i].rssi << ","
          << "\"security\":\"" << escape_json(results[i].security) << "\"}";
    }
    o << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string cfg_path = "config.ini";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--config" || a == "-c") && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " [--config config.ini]\n";
            return 0;
        }
    }

    Config cfg(cfg_path);
    const std::string adapter    = cfg.get_str("bluetooth.adapter", "hci0");
    const std::string dev_name   = cfg.get_str("bluetooth.device_name", "pi-bluetooth-configuration");
    const std::string iface      = cfg.get_str("wifi.interface", "wlan0");
    const int max_scan_results   = cfg.get_int("scan.max_results", 10);
    const int network_port       = cfg.get_int("network.port", 8567);
    const std::string adapter_path = "/org/bluez/" + adapter;

    std::cerr << "[Config] adapter  : " << adapter_path << "\n"
              << "[Config] device   : " << dev_name << "\n"
              << "[Config] wifi if  : " << iface << "\n"
              << "[Config] tcp port : " << network_port << "\n";

    dbus_threads_init_default();

    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err) || !conn) {
        std::cerr << "[DBus] failed to connect to system bus: " << err.message << "\n";
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(conn, 0);

    WifiControl wifi(iface);

    gattsrv::GattServer server(conn, adapter_path, APP_ROOT, SERVICE_UUID, dev_name);

    std::mutex staged_mu;
    std::string staged_ssid, staged_psk;

    std::mutex scan_mu;
    std::string last_scan_json = "[]";

    server.add_characteristic(SSID_UUID, {"write"}, nullptr,
        [&](const std::vector<uint8_t>& v) {
            std::lock_guard<std::mutex> lk(staged_mu);
            staged_ssid.assign(v.begin(), v.end());
        });

    server.add_characteristic(PSK_UUID, {"write"}, nullptr,
        [&](const std::vector<uint8_t>& v) {
            std::lock_guard<std::mutex> lk(staged_mu);
            staged_psk.assign(v.begin(), v.end());
        });

    gattsrv::Characteristic* status_char = server.add_characteristic(
        STATUS_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(status_json(wifi.get_status())); },
        nullptr);

    gattsrv::Characteristic* scan_char = server.add_characteristic(
        SCAN_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> {
            std::lock_guard<std::mutex> lk(scan_mu);
            return to_bytes(last_scan_json);
        },
        nullptr);

    // Shared by both the BLE Command characteristic and the TCP control
    // interface below, so "scan"/"connect"/"forget" behave identically
    // and update the same state no matter which channel triggered them.
    auto do_scan = [&]() {
        auto results = wifi.scan(max_scan_results);
        std::string json;
        {
            std::lock_guard<std::mutex> lk(scan_mu);
            last_scan_json = scan_json(results);
            json = last_scan_json;
        }
        server.notify(scan_char, to_bytes(json));
    };

    auto do_connect = [&](std::string ssid, std::string psk) {
        wifi.connect(ssid, psk);
        server.notify(status_char, to_bytes(status_json(wifi.get_status())));
    };

    auto do_forget = [&]() {
        wifi.forget();
        server.notify(status_char, to_bytes(status_json(wifi.get_status())));
    };

    auto get_status_json = [&]() -> std::string { return status_json(wifi.get_status()); };
    auto get_scan_json = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(scan_mu);
        return last_scan_json;
    };

    server.add_characteristic(COMMAND_UUID, {"write"}, nullptr,
        [&](const std::vector<uint8_t>& v) {
            std::string cmd = trim(std::string(v.begin(), v.end()));

            if (cmd == "scan") {
                std::thread([&]() { InflightGuard guard; do_scan(); }).detach();

            } else if (cmd == "connect") {
                std::string ssid, psk;
                {
                    std::lock_guard<std::mutex> lk(staged_mu);
                    ssid = staged_ssid;
                    psk = staged_psk;
                    std::fill(staged_psk.begin(), staged_psk.end(), '\0');
                    staged_psk.clear();
                }
                std::thread([&, ssid, psk]() { InflightGuard guard; do_connect(ssid, psk); }).detach();

            } else if (cmd == "forget") {
                do_forget();

            } else {
                std::cerr << "[Command] unrecognised command: " << cmd << "\n";
            }
        });

    std::string start_err;
    if (!server.start(start_err)) {
        std::cerr << "[BlueZ] failed to start GATT server: " << start_err << "\n";
        return 1;
    }
    std::cerr << "[BlueZ] advertising as \"" << dev_name << "\", service " << SERVICE_UUID << "\n";

    // Always-on handoff target: once Status reports a non-empty ip, a
    // well-behaved client should switch to this instead of continuing to
    // rely on BLE (see the file header comment for why).
    tcpctl::TcpControlServer tcp;
    tcp.on_status = get_status_json;
    tcp.on_scan_results = get_scan_json;
    tcp.on_scan = [&]() { std::thread([&]() { InflightGuard guard; do_scan(); }).detach(); };
    tcp.on_connect = [&](std::string ssid, std::string psk) {
        std::thread([&, ssid, psk]() { InflightGuard guard; do_connect(ssid, psk); }).detach();
    };
    tcp.on_forget = [&]() { do_forget(); };
    tcp.start(network_port);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (g_running) {
        dbus_connection_read_write_dispatch(conn, 200);
    }

    std::cerr << "[Main] shutting down, waiting for in-flight scan/connect work...\n";
    while (g_inflight.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}
