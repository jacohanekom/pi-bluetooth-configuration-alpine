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
 *   EthernetIP  (read + write)   stage/read eth0's "ip,rangeStart,rangeEnd" (see eth_control.hpp)
 *   DhcpLeases  (read + notify)  [{"ip":...,"mac":...,"hostname":...}, ...] currently on eth0
 *   Command     (write)          "scan" | "connect" | "forget" | "set_ethernet" | "finish" | "relay <port> on|off"
 *   Status      (read + notify)  {"state":...,"ssid":...,"ip":...,"error":...,"finished":...}
 *   ScanResults (read + notify)  [{"ssid":...,"rssi":...,"security":...}, ...]
 *   Relays      (read + notify)  [{"port":...,"label":...,"state":...}, ...] -- see relay_control.hpp
 *   Victron     (read + notify)  {"connected":...,"device":{...},"V":...,...} -- see victron_control.hpp
 *
 * eth0 is always a working gateway: its static IP + DHCP server are
 * (re)applied directly at every startup -- independent of dhcpcd and
 * carrier state -- so a Pi is reachable over Ethernet with no app
 * interaction, cable plugged in or not. "set_ethernet" lets it be
 * customized during the setup wizard -- including the step right after
 * WiFi connects, before "finish" is sent -- but once "finish" actually
 * runs (marking the wizard done and rebooting), this daemon rejects
 * further changes and the app switches to a read-only display instead
 * (see eth_control.hpp and the README's "Ethernet direct-connect"
 * section). Unlike WiFi, applying this doesn't reboot: Ethernet doesn't
 * share the Pi 3's antenna with Bluetooth, so there's no coexistence
 * problem to route around, and the change is visible immediately.
 *
 * "connect" itself no longer reboots on success -- the wizard has one
 * more optional step (local network configuration) after WiFi joins,
 * and only "finish" actually marks the device done and reboots it.
 *
 * No pairing/bonding: characteristics are plain read/write, not
 * encrypted. An earlier revision required BLE pairing, but bonding on the
 * Pi 3's BCM43438 proved unreliable enough in practice (bond desync
 * between BlueZ and the central, repeated OS-level pairing prompts) that
 * it wasn't worth what it protected against -- see the README's Security
 * model section. This means WiFi credentials cross BLE in the clear;
 * treat this as suitable for a trusted home/lab network, not a public one.
 *
 * Relay control is a separate, optional integration with
 * pi-relay-control-alpine: this daemon doesn't drive GPIO itself, it just
 * forwards "relay <port> on|off" (from Command) to whichever relay is
 * listening on that TCP port on 127.0.0.1, and reports live state back
 * on the Relays characteristic. It's no part of the setup wizard --
 * relay commands are rejected and Relays reports an empty list until
 * MARKER_FILE exists, i.e. only once setup has actually finished, the
 * same point at which the app switches to showing WiFi/network stats
 * (relay controls belong on that same screen, not the wizard). See
 * relay_control.hpp and the README's "Relay control" section for the
 * "[relays]" config format that maps ports to display labels.
 *
 * Victron solar/battery telemetry is a similar optional integration,
 * this time with victron-ve-direct-alpine: queries its status control
 * port for the latest reading and republishes it as JSON on the Victron
 * characteristic. Unlike relay control this is read-only (nothing to
 * gate against acting on an unprovisioned Pi) and isn't tied to
 * MARKER_FILE at all -- it's always live, the app just chooses to show
 * it on the same post-setup screen as WiFi/network stats and relays.
 * See victron_control.hpp.
 *
 * Advertised as the board's hardware serial (from /proc/cpuinfo), not a
 * fixed name, so a client's device list distinguishes between multiple
 * aipicam units instead of showing the same string for all of them.
 *
 * This is a one-shot provisioning flow, not a managed session: a
 * successful "connect" creates MARKER_FILE and reboots the Pi a few
 * seconds later (enough time for the final Status notification to reach
 * the client first); a "forget" removes MARKER_FILE and reboots the same
 * way. There is deliberately no ongoing management interface beyond BLE
 * -- once WiFi is set up, the expectation is that the Pi reboots into its
 * normal role, not that a client keeps talking to this daemon.
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
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "config.hpp"
#include "eth_control.hpp"
#include "gatt_server.hpp"
#include "relay_control.hpp"
#include "victron_control.hpp"
#include "wifi_control.hpp"

namespace {

constexpr const char* SERVICE_UUID  = "7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* SSID_UUID     = "7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* PSK_UUID      = "7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* COMMAND_UUID  = "7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* STATUS_UUID   = "7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* SCAN_UUID     = "7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* ETH_IP_UUID   = "7b1e0006-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* LEASES_UUID   = "7b1e0007-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* RELAYS_UUID   = "7b1e0008-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* VICTRON_UUID  = "7b1e0009-6a45-4d1f-9b0a-3c2f8e4d5a10";
constexpr const char* APP_ROOT      = "/org/bluez/pibtconf";
constexpr const char* MARKER_FILE   = "/.successfully-initialized";
constexpr int REBOOT_DELAY_SECS     = 3;

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

// "finished" reflects whether this device has already completed the
// one-shot provisioning wizard (MARKER_FILE exists) -- the client uses
// this, not just wifi state, to decide whether to show the wizard or
// the final read-only details screen.
std::string status_json(const WifiStatus& s, bool finished) {
    std::ostringstream o;
    o << "{\"state\":\"" << s.state_name() << "\","
      << "\"ssid\":\"" << escape_json(s.ssid) << "\","
      << "\"ip\":\"" << escape_json(s.ip) << "\","
      << "\"error\":\"" << escape_json(s.error) << "\","
      << "\"finished\":" << (finished ? "true" : "false") << "}";
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

std::string leases_json(const std::vector<ethctl::Lease>& leases) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < leases.size(); ++i) {
        if (i) o << ",";
        std::string hostname = leases[i].hostname == "*" ? "" : leases[i].hostname;
        o << "{\"ip\":\"" << escape_json(leases[i].ip) << "\","
          << "\"mac\":\"" << escape_json(leases[i].mac) << "\","
          << "\"hostname\":\"" << escape_json(hostname) << "\"}";
    }
    o << "]";
    return o.str();
}

// Queries each configured relay's live state (via relay_control.hpp,
// one TCP round-trip per relay to pi-relay-control-alpine) every time
// this is called -- simple, and there are only ever a handful of
// relays, so this is cheap enough to run both on-demand (a GATT read)
// and on the poll/notify thread below.
// port_mu locks each relay individually (just around that relay's own
// query) rather than one lock for the whole sweep -- see do_relay for
// why a lock is needed at all, and relay_port_mu's own comment for why
// it's per-port. A short, per-relay critical section here means a slow
// query against one relay still can't block a command to a different
// one from this same loop either.
std::string relays_json(const std::vector<relayctl::RelayConfig>& relays,
                         std::map<int, std::mutex>& port_mu) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < relays.size(); ++i) {
        if (i) o << ",";
        std::string state;
        {
            std::lock_guard<std::mutex> lk(port_mu[relays[i].port]);
            state = relayctl::query_state(relays[i].port);
        }
        o << "{\"port\":" << relays[i].port << ","
          << "\"label\":\"" << escape_json(relays[i].label) << "\","
          << "\"state\":\"" << state << "\"}";
    }
    o << "]";
    return o.str();
}

// Shapes a fresh query of victron-ve-direct-alpine's status port into
// JSON, using the same field names as that project's own data_port
// telemetry frames (see its README) so a client only needs to learn one
// schema for this data. "connected":false (with nothing else present)
// covers every failure case -- not installed, not running, or up but
// hasn't synced a VE.Direct frame yet -- since a client only ever needs
// to know "is there anything to show".
std::string victron_json(const victronctl::VictronStatus& s) {
    std::ostringstream o;
    o << "{\"connected\":" << (s.connected ? "true" : "false");
    if (s.connected) {
        o << ",\"device\":{"
          << "\"pid\":\"" << escape_json(s.pid) << "\","
          << "\"name\":\"" << escape_json(s.device_name) << "\","
          << "\"serial\":\"" << escape_json(s.serial) << "\","
          << "\"fw\":\"" << escape_json(s.fw) << "\"},"
          << "\"V\":" << s.V << ","
          << "\"I\":" << s.I << ","
          << "\"VPV\":" << s.VPV << ","
          << "\"PPV\":" << s.PPV << ","
          << "\"CS\":" << s.CS << ","
          << "\"CS_name\":\"" << escape_json(s.CS_name) << "\","
          << "\"ERR\":" << s.ERR << ","
          << "\"ERR_name\":\"" << escape_json(s.ERR_name) << "\","
          << "\"H20\":" << s.H20;
    }
    o << "}";
    return o.str();
}

bool marker_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

// EthernetIP's wire format is plain CSV, "<ip>,<rangeStart>,<rangeEnd>"
// (e.g. "192.168.4.1,2,200") both ways -- simple enough not to need a
// real JSON parser for the write side, and the daemon has no JSON
// parsing anywhere else either.
std::string eth_config_csv(const ethctl::EthControl& eth) {
    auto cfg = eth.get_config();
    std::string ip = eth.get_ip();
    if (ip.empty()) ip = cfg.ip;
    std::ostringstream o;
    o << ip << "," << cfg.range_start << "," << cfg.range_end;
    return o.str();
}

bool parse_eth_csv(const std::string& csv, std::string& ip, int& range_start, int& range_end) {
    std::stringstream ss(csv);
    std::string ip_s, start_s, end_s;
    if (!std::getline(ss, ip_s, ',')) return false;
    if (!std::getline(ss, start_s, ',')) return false;
    if (!std::getline(ss, end_s, ',')) return false;
    try {
        range_start = std::stoi(start_s);
        range_end = std::stoi(end_s);
    } catch (...) {
        return false;
    }
    ip = trim(ip_s);
    return true;
}

// The board's hardware serial (from /proc/cpuinfo) rather than a fixed
// configured name, so a client's "nearby devices" list distinguishes
// between multiple aipicam units instead of showing the same string for
// all of them. Falls back to bluetooth.device_name (e.g. when not
// running on real Pi hardware) if it can't be read.
std::string read_pi_serial() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("Serial");
        if (pos == std::string::npos) continue;
        auto colon = line.find(':', pos);
        if (colon == std::string::npos) continue;
        std::string serial = trim(line.substr(colon + 1));
        if (!serial.empty()) return serial;
    }
    return "";
}

// Waits long enough for the just-sent BLE notification to actually reach
// the client before the reboot drops the connection, then reboots.
// Fire-and-forget: once the reboot command is issued, the whole system
// (including this process) is going down regardless of what run_command
// reports back.
void reboot_after_delay() {
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(REBOOT_DELAY_SECS));
        std::cerr << "[Main] rebooting now\n";
        run_command({"reboot"});
    }).detach();
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
    const std::string configured_name = cfg.get_str("bluetooth.device_name", "pi-bluetooth-configuration");
    const std::string serial     = read_pi_serial();
    const std::string dev_name   = serial.empty() ? configured_name : serial;
    const std::string iface      = cfg.get_str("wifi.interface", "wlan0");
    const std::string eth_iface  = cfg.get_str("ethernet.interface", "eth0");
    const std::string eth_default_ip = cfg.get_str("ethernet.ip", "192.168.4.1");
    const int eth_default_range_start = cfg.get_int("ethernet.dhcp_range_start", 2);
    const int eth_default_range_end   = cfg.get_int("ethernet.dhcp_range_end", 200);
    const int max_scan_results   = cfg.get_int("scan.max_results", 10);
    const auto relays = relayctl::load_relays(cfg_path);

    // One mutex per configured relay port -- populated once, up front,
    // before any thread that might read it starts, so every later
    // relay_port_mu[port] lookup below only ever finds an existing key
    // and never triggers a concurrent std::map insert/rehash. Keyed per
    // port (not one mutex for every relay) so a manual on/off command
    // to one relay is never blocked behind a slow or stuck query against
    // a completely different one -- see relays_json/do_relay below for
    // why a lock is needed here at all.
    std::map<int, std::mutex> relay_port_mu;
    for (const auto& r : relays) relay_port_mu[r.port];
    const int victron_ctrl_port = cfg.get_int("victron.ctrl_port", 8562);
    const std::string adapter_path = "/org/bluez/" + adapter;

    std::cerr << "[Config] adapter  : " << adapter_path << "\n"
              << "[Config] device   : " << dev_name << (serial.empty() ? " (configured)" : " (hardware serial)") << "\n"
              << "[Config] wifi if  : " << iface << "\n"
              << "[Config] eth if   : " << eth_iface << "\n"
              << "[Config] relays   : " << relays.size() << " configured\n"
              << "[Config] victron  : ctrl_port " << victron_ctrl_port << "\n";

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
    ethctl::EthControl eth(eth_iface);

    // eth0 is meant to always be a usable gateway, not something the app
    // has to configure first -- reapply its static IP/range (whatever
    // was last chosen, or the configured defaults on a fresh install)
    // on every startup, since `ip addr add` doesn't survive a reboot.
    // Once that's in place, NAT eth0's traffic out through WiFi (see
    // eth_control.hpp's enable_internet_sharing) so a device plugged
    // into eth0 gets real internet access, not just a link to the Pi
    // itself -- WiFi is what actually has the internet connection here.
    std::thread([&eth, eth_default_ip, eth_default_range_start, eth_default_range_end, eth_iface, iface]() {
        InflightGuard guard;
        std::string ip_err;
        if (!eth.ensure_static_ip(eth_default_ip, eth_default_range_start, eth_default_range_end, ip_err)) {
            std::cerr << "[Ethernet] failed to apply gateway IP: " << ip_err << "\n";
        }
        std::string nat_err;
        if (!ethctl::enable_internet_sharing(eth_iface, iface, nat_err)) {
            std::cerr << "[Ethernet] failed to enable internet sharing (" << eth_iface << " -> " << iface << "): " << nat_err << "\n";
        }
    }).detach();

    gattsrv::GattServer server(conn, adapter_path, APP_ROOT, SERVICE_UUID, dev_name);

    std::mutex staged_mu;
    std::string staged_ssid, staged_psk, staged_eth_ip;

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

    gattsrv::Characteristic* eth_ip_char = server.add_characteristic(
        ETH_IP_UUID, {"read", "write", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(eth_config_csv(eth)); },
        [&](const std::vector<uint8_t>& v) {
            std::lock_guard<std::mutex> lk(staged_mu);
            staged_eth_ip.assign(v.begin(), v.end());
        });

    gattsrv::Characteristic* leases_char = server.add_characteristic(
        LEASES_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(leases_json(eth.get_leases())); },
        nullptr);

    // Relays are no part of the setup wizard -- they only ever report
    // real state once the Pi has actually finished setup (MARKER_FILE
    // exists), the same point at which the app's details screen starts
    // showing WiFi/network stats and, alongside them, relay controls.
    // Before that, this reports an empty list rather than querying
    // pi-relay-control-alpine at all -- that daemon's own start_pre()
    // gate (see its README, "Requires device provisioning") means
    // there's nothing listening on those ports yet anyway.
    auto current_relays_json = [&]() -> std::string {
        return marker_exists(MARKER_FILE) ? relays_json(relays, relay_port_mu) : "[]";
    };

    gattsrv::Characteristic* relays_char = server.add_characteristic(
        RELAYS_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(current_relays_json()); },
        nullptr);

    // Unlike Relays, Victron telemetry is read-only and isn't gated by
    // MARKER_FILE -- there's no action to hold back before setup
    // finishes, just a reading to report (or not, if nothing's
    // reachable yet). The app chooses to surface it on the same
    // post-setup screen as WiFi/network stats and relays; this
    // characteristic itself is live from the moment BLE comes up.
    gattsrv::Characteristic* victron_char = server.add_characteristic(
        VICTRON_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(victron_json(victronctl::query_status(victron_ctrl_port))); },
        nullptr);

    gattsrv::Characteristic* status_char = server.add_characteristic(
        STATUS_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> { return to_bytes(status_json(wifi.get_status(), marker_exists(MARKER_FILE))); },
        nullptr);

    gattsrv::Characteristic* scan_char = server.add_characteristic(
        SCAN_UUID, {"read", "notify"},
        [&]() -> std::vector<uint8_t> {
            std::lock_guard<std::mutex> lk(scan_mu);
            return to_bytes(last_scan_json);
        },
        nullptr);

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

    // Joining WiFi no longer reboots by itself -- the wizard has one
    // more step after this (local network configuration), and the whole
    // thing only concludes, and reboots, once "finish" is sent. See
    // do_finish below.
    auto do_connect = [&](std::string ssid, std::string psk) {
        wifi.connect(ssid, psk);
        server.notify(status_char, to_bytes(status_json(wifi.get_status(), marker_exists(MARKER_FILE))));
    };

    auto do_forget = [&]() {
        wifi.forget();
        server.notify(status_char, to_bytes(status_json(wifi.get_status(), false)));
        std::remove(MARKER_FILE);
        reboot_after_delay();
    };

    // The end of the wizard: only meaningful once WiFi is actually
    // connected, since finishing before that would reboot into a Pi
    // that isn't actually configured. Marks success on disk and reboots
    // into the Pi's normal role rather than staying up to be managed
    // further.
    auto do_finish = [&]() {
        if (wifi.get_status().state != WifiStatus::CONNECTED) {
            std::cerr << "[Finish] ignoring: WiFi is not connected\n";
            return;
        }
        std::ofstream(MARKER_FILE).close();
        server.notify(status_char, to_bytes(status_json(wifi.get_status(), true)));
        reboot_after_delay();
    };

    // Ethernet direct-connect is only reconfigurable until the wizard
    // finishes (MARKER_FILE doesn't exist yet) -- that includes the
    // window right after WiFi connects but before "finish" is sent.
    // Once finished, eth0's gateway config is left as-is and the app
    // switches to a read-only display of it. Applied immediately when
    // allowed, no reboot needed (see file header comment).
    auto do_set_ethernet = [&](std::string ip, int range_start, int range_end) {
        if (marker_exists(MARKER_FILE)) {
            std::cerr << "[Ethernet] ignoring set_ethernet: setup has already finished\n";
            return;
        }
        std::string ip_err;
        if (!eth.set_static_ip(ip, range_start, range_end, ip_err)) {
            std::cerr << "[Ethernet] failed to set static IP " << ip << ": " << ip_err << "\n";
        }
        server.notify(eth_ip_char, to_bytes(eth_config_csv(eth)));
    };

    // Relays are no part of the setup wizard -- they're only meaningful
    // once the Pi is actually set up, at which point the app's details
    // screen (the same one showing WiFi/network stats) offers them
    // alongside it. Refusing here isn't just UI-side politeness: this
    // mirrors pi-relay-control-alpine's own start_pre() gate, which
    // refuses to run at all until MARKER_FILE exists (see that repo's
    // README, "Requires device provisioning") -- before that point,
    // there's no relay daemon on the other end of the socket to command
    // in the first place.
    //
    // Forwards "relay <port> on|off" to whichever relay
    // pi-relay-control-alpine has listening on that TCP port (see
    // relay_control.hpp) and pushes the refreshed Relays state back to
    // the client either way -- including on failure (state comes back
    // "unknown"), so the UI reflects reality rather than optimistically
    // assuming the write worked.
    auto do_relay = [&](int port, const std::string& action) {
        if (!marker_exists(MARKER_FILE)) {
            std::cerr << "[Relay] ignoring relay command: setup has not finished yet\n";
            return;
        }
        // relay_port_mu is pre-populated once at startup with exactly the
        // configured ports (see its declaration above) so every lookup
        // below is a plain read on an existing key, never a concurrent
        // std::map insert racing the periodic poll thread's own lookups.
        // port comes straight from the client's write, unvalidated, so
        // that guarantee only holds if an unconfigured port is rejected
        // here first, before ever touching the map.
        bool configured = std::any_of(relays.begin(), relays.end(),
                                       [&](const relayctl::RelayConfig& r) { return r.port == port; });
        if (!configured) {
            std::cerr << "[Relay] ignoring relay command for unconfigured port " << port << "\n";
            return;
        }
        // Retries until pi-relay-control-alpine actually confirms the
        // change ("OK RELAY=ON"/"OK RELAY=OFF") rather than accepting the
        // first attempt regardless of outcome -- a transient failure
        // (relay busy, a dropped connection, etc.) would otherwise
        // silently leave the relay unchanged with only a log line to show
        // for it. Bounded by both an attempt count and a wall-clock
        // budget so a persistently unreachable relay still gives up
        // rather than retrying forever.
        constexpr int max_attempts = 5;
        constexpr auto retry_delay = std::chrono::milliseconds(300);
        constexpr auto max_total_time = std::chrono::seconds(10);
        auto deadline = std::chrono::steady_clock::now() + max_total_time;

        std::string err, resp;
        bool confirmed = false;
        int attempt = 0;
        while (!confirmed && ++attempt <= max_attempts && std::chrono::steady_clock::now() < deadline) {
            {
                // Locked only around the command itself, on just this
                // port's mutex -- not across the current_relays_json()
                // call below, which locks each port (including this one)
                // again itself. Holding it across both would self-deadlock
                // on this thread re-locking a non-recursive mutex it
                // already owns. The write has already fully applied by
                // the time this unlocks, so any query that lands after --
                // from that call or the periodic poll thread -- sees the
                // real post-command state regardless; there's no
                // staleness window left to protect against once the
                // command itself has finished.
                std::lock_guard<std::mutex> lk(relay_port_mu[port]);
                resp = relayctl::send_command(port, action, err);
            }
            confirmed = err.empty() && resp.rfind("OK", 0) == 0;
            if (!confirmed && attempt < max_attempts) {
                std::cerr << "[Relay] port " << port << " " << action << " attempt " << attempt << "/"
                           << max_attempts << " -> " << (err.empty() ? resp : err) << ", retrying\n";
                std::this_thread::sleep_for(retry_delay);
            }
        }

        if (confirmed) {
            std::cerr << "[Relay] port " << port << " " << action << " -> " << resp
                       << " (attempt " << attempt << "/" << max_attempts << ")\n";
        } else if (!err.empty()) {
            std::cerr << "[Relay] port " << port << " " << action << " gave up after " << attempt
                       << " attempts: " << err << "\n";
        } else {
            std::cerr << "[Relay] port " << port << " " << action << " gave up after " << attempt
                       << " attempts, last response: " << resp << "\n";
        }
        server.notify(relays_char, to_bytes(current_relays_json()));
    };

    server.add_characteristic(COMMAND_UUID, {"write"}, nullptr,
        [&](const std::vector<uint8_t>& v) {
            std::string cmd = trim(std::string(v.begin(), v.end()));
            // Unconditional, logged before any parsing/dispatch below --
            // every well-formed command otherwise only logs deep inside
            // its own handler (e.g. do_relay), so a write that reaches
            // BlueZ/D-Bus but somehow never gets this far would otherwise
            // leave no trace at all. If this line is ever missing for a
            // command the client believes it sent, the write never
            // reached the daemon in the first place -- not a bug in any
            // of the handlers below, but in the BLE link itself.
            std::cerr << "[Command] received: \"" << cmd << "\"\n";

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

            } else if (cmd == "finish") {
                do_finish();

            } else if (cmd == "set_ethernet") {
                std::string csv;
                {
                    std::lock_guard<std::mutex> lk(staged_mu);
                    csv = staged_eth_ip;
                }
                std::string ip;
                int range_start, range_end;
                if (!parse_eth_csv(csv, ip, range_start, range_end)) {
                    std::cerr << "[Ethernet] malformed set_ethernet payload: " << csv << "\n";
                } else {
                    std::thread([&, ip, range_start, range_end]() {
                        InflightGuard guard;
                        do_set_ethernet(ip, range_start, range_end);
                    }).detach();
                }

            } else if (cmd.rfind("relay ", 0) == 0) {
                std::istringstream iss(cmd.substr(6));
                int port = 0;
                std::string action;
                if (!(iss >> port >> action) || (action != "on" && action != "off")) {
                    std::cerr << "[Relay] malformed command, expected: relay <port> on|off (" << cmd << ")\n";
                } else {
                    std::thread([&, port, action]() { InflightGuard guard; do_relay(port, action); }).detach();
                }

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

    // Polls dnsmasq's leases file for changes so a connected client's
    // "allocated IPs" view updates on its own as devices join/leave
    // eth0, without needing to re-open the app to see it.
    std::thread([&]() {
        std::string last;
        while (g_running) {
            std::string json = leases_json(eth.get_leases());
            if (json != last) {
                last = json;
                server.notify(leases_char, to_bytes(json));
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();

    // Polls each configured relay's live state so a connected client
    // sees changes made from elsewhere (another client, always_on
    // resuming after pi-relay-control-alpine restarts, etc.) without
    // having to trigger a write itself first. Skipped entirely when no
    // relays are configured, since current_relays_json() would never
    // change either way. Before setup finishes, current_relays_json()
    // reports "[]" without touching the network -- see relays_char above
    // -- so this doesn't start hammering pi-relay-control-alpine (which
    // isn't even running yet) until there's an actual reason to.
    if (!relays.empty()) {
        std::thread([&]() {
            std::string last;
            while (g_running) {
                // current_relays_json() -> relays_json() locks each port
                // individually around its own query -- no outer lock
                // needed here, and one would only add cross-port blocking
                // this loop doesn't need (see relay_port_mu's comment).
                std::string json = current_relays_json();
                if (json != last) {
                    last = json;
                    server.notify(relays_char, to_bytes(json));
                }
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }).detach();
    }

    // Polls victron-ve-direct-alpine's status port for the latest
    // telemetry frame so a connected client's Solar/Battery view
    // updates on its own -- same pattern as the leases/relays polling
    // above. Harmless when victron-ve-direct-alpine isn't installed:
    // each attempt just fails to connect (see victron_control.hpp),
    // which only ever notifies once (the initial "connected":false),
    // not repeatedly, since this only notifies on change.
    std::thread([&]() {
        std::string last;
        while (g_running) {
            std::string json = victron_json(victronctl::query_status(victron_ctrl_port));
            if (json != last) {
                last = json;
                server.notify(victron_char, to_bytes(json));
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (g_running) {
        dbus_connection_read_write_dispatch(conn, 200);
    }

    std::cerr << "[Main] shutting down, waiting for in-flight scan/connect work...\n";
    while (g_inflight.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}
