/**
 * pi-bluetooth-configuration
 * ===========================
 * Lets a phone/app join this Pi to a WiFi network without SSH or a
 * keyboard, using the Pi's own onboard WiFi radio for both roles at
 * different times -- never BLE, never a second board. On startup this
 * daemon tries to join whatever's already configured (wpa_supplicant,
 * started by OpenRC before this daemon, attempts that entirely on its
 * own); if that doesn't succeed within a bounded time -- including the
 * common case of nothing being configured at all yet -- wlan0 switches
 * into its own access point (see ap_control.hpp) that a phone can join
 * directly, reaching this daemon's HTTP API (see http_server.hpp) to
 * submit real credentials -- found automatically via mDNS/Bonjour (see
 * mdns_responder.hpp) rather than requiring its address to be typed in,
 * on whichever network (the fallback AP, or a real one once joined) the
 * phone happens to be on.
 *
 * This replaces two earlier designs entirely (see git history): a
 * direct BlueZ/D-Bus GATT peripheral, and -- after BlueZ's own built-in
 * GATT profiles proved to force a disconnect loop no userspace config
 * could fix -- offloading BLE to a Raspberry Pi Pico 2 W over USB
 * serial. Both were scrapped in favor of this: a plain WiFi AP + HTTP
 * API is a far more standard, battle-tested pattern for headless device
 * setup (the same shape ESP8266/ESP32 WiFiManager-style devices use),
 * and it reuses infrastructure (hostapd, dnsmasq) this project already
 * needed for other things.
 *
 * HTTP API (JSON; see the route table in main() for the exact shapes):
 *   GET  /status    combined snapshot -- wifi state, whether AP mode is
 *                   currently active, eth0's config, DHCP leases, relay
 *                   states, Victron telemetry, and the last scan's
 *                   results. No server push (unlike the old BLE
 *                   notify): clients are expected to poll this
 *                   periodically instead -- simpler and more robust
 *                   than the notify-plus-cache machinery either earlier
 *                   design needed.
 *   POST /scan      triggers a background WiFi scan; poll GET /status
 *                   for results once it finishes (a few seconds later).
 *   POST /connect   {"ssid":...,"password":...} -- see do_connect's own
 *                   comment for why this behaves differently depending
 *                   on whether AP mode is currently active.
 *   POST /forget    forgets the configured network, reboots.
 *   POST /finish    concludes setup (only meaningful when NOT starting
 *                   from AP mode -- see do_connect).
 *   GET  /ethernet  current eth0 gateway IP + DHCP range.
 *   POST /ethernet  {"ip":...,"rangeStart":...,"rangeEnd":...} -- stage
 *                   eth0's local network config; see eth_control.hpp.
 *   POST /relay     {"port":...,"state":"on"|"off"} -- see relay_control.hpp.
 *
 * eth0 is always a working gateway: its static IP + DHCP server are
 * (re)applied directly at every startup -- independent of dhcpcd,
 * carrier state, and whatever wlan0 is currently doing -- so a Pi is
 * reachable over Ethernet with no app interaction, cable plugged in or
 * not. POST /ethernet lets it be customized any time before "finish"
 * actually runs (marking the wizard done and rebooting); after that this
 * daemon rejects further changes and the app switches to a read-only
 * display instead (see eth_control.hpp and the README's "Ethernet
 * direct-connect" section). Applying it doesn't reboot: Ethernet doesn't
 * share the radio with wlan0, so there's no coexistence problem to route
 * around, and the change is visible immediately.
 *
 * No auth on this daemon's own HTTP API: requests are plain HTTP, not
 * encrypted or authenticated. The AP itself is open (no password) too --
 * seem this project's security model (see the README) already treats
 * the WiFi-configuration flow as suitable for a trusted home/lab
 * environment only, not a public one; this means WiFi credentials cross
 * both the AP's own air interface and this HTTP API in the clear.
 *
 * Relay control is a separate, optional integration with
 * pi-relay-control-alpine: this daemon doesn't drive GPIO itself, it
 * just forwards on/off to whichever relay is listening on that TCP port
 * on 127.0.0.1, and reports live state back via GET /status. It's no
 * part of the setup wizard -- relay commands are rejected and
 * GET /status reports an empty relay list until MARKER_FILE exists,
 * i.e. only once setup has actually finished, the same point at which
 * the app switches to showing WiFi/network stats (relay controls belong
 * on that same screen, not the wizard). See relay_control.hpp and the
 * README's "Relay control" section for the "[relays]" config format
 * that maps ports to display labels.
 *
 * Victron solar/battery telemetry is a similar optional integration,
 * this time with victron-ve-direct-alpine: queries its status control
 * port for the latest reading and republishes it as JSON. Unlike relay
 * control this is read-only (nothing to gate against acting on an
 * unprovisioned Pi) and isn't tied to MARKER_FILE at all -- it's always
 * live, the app just chooses to show it on the same post-setup screen as
 * WiFi/network stats and relays. See victron_control.hpp.
 *
 * The AP's own SSID is the board's hardware serial (from /proc/cpuinfo),
 * not a fixed name, so multiple aipicam units are distinguishable in a
 * phone's WiFi network list instead of all showing the same name.
 *
 * This is a one-shot provisioning flow, not a managed session: a
 * successful "finish" (or a "connect" that started from AP mode -- see
 * do_connect) creates MARKER_FILE and reboots the Pi a few seconds
 * later; a "forget" removes MARKER_FILE and reboots the same way. There
 * is deliberately no ongoing management interface beyond this same HTTP
 * API -- once WiFi is set up, the expectation is that the Pi reboots
 * into its normal role, and this daemon (and the API) stay available on
 * the joined network for the relay/Victron/status use described above,
 * not for repeating the wizard.
 *
 * Build (Alpine Linux):
 *   make
 * Run:
 *   ./pi-bluetooth-configuration [--config config.ini]
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ap_control.hpp"
#include "config.hpp"
#include "eth_control.hpp"
#include "http_server.hpp"
#include "mdns_responder.hpp"
#include "relay_control.hpp"
#include "victron_control.hpp"
#include "wifi_control.hpp"

namespace {

constexpr const char* MARKER_FILE = "/.successfully-initialized";
constexpr int REBOOT_DELAY_SECS = 3;

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

// Bare-bones flat-JSON-object value extraction -- good enough for this
// daemon's own small, flat POST bodies ({"ssid":"...","password":"..."}
// and similar), not a general parser. No nesting, no arrays -- nothing
// this daemon's own API ever needs to receive uses either.
std::string json_get_string(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = body.find('"', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    std::string out;
    while (pos < body.size() && body[pos] != '"') {
        if (body[pos] == '\\' && pos + 1 < body.size()) {
            char esc = body[pos + 1];
            switch (esc) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                default: out += esc;
            }
            pos += 2;
        } else {
            out += body[pos];
            ++pos;
        }
    }
    return out;
}

long json_get_int(const std::string& body, const std::string& key, long def) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return def;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    size_t start = pos;
    if (pos < body.size() && (body[pos] == '-' || body[pos] == '+')) ++pos;
    while (pos < body.size() && std::isdigit(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos == start) return def;
    try { return std::stol(body.substr(start, pos - start)); } catch (...) { return def; }
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

bool marker_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

// Queries each configured relay's live state (via relay_control.hpp,
// one TCP round-trip per relay to pi-relay-control-alpine) every time
// this is called -- simple, and there are only ever a handful of
// relays, so this is cheap enough to run on every GET /status. Unlike
// the old BLE-era design, there's no shared dispatch thread this could
// block (http_server.hpp is thread-per-connection -- a slow relay query
// only ever delays its own request), so the caching layer that used to
// exist here for exactly that reason is gone.
std::string relays_json(const std::vector<relayctl::RelayConfig>& relays,
                         std::map<int, std::mutex>& port_mu) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < relays.size(); ++i) {
        if (i) o << ",";
        std::string state;
        {
            // Still locked per-port -- not to protect against a shared
            // dispatch thread anymore, just so a concurrent GET /status
            // and POST /relay for the *same* port (two separate HTTP
            // connections, genuinely concurrent threads now) can't
            // interleave their TCP conversations with
            // pi-relay-control-alpine.
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

std::string eth_config_json(const ethctl::EthControl& eth) {
    auto cfg = eth.get_config();
    std::string ip = eth.get_ip();
    if (ip.empty()) ip = cfg.ip;
    std::ostringstream o;
    o << "{\"ip\":\"" << escape_json(ip) << "\","
      << "\"rangeStart\":" << cfg.range_start << ","
      << "\"rangeEnd\":" << cfg.range_end << "}";
    return o.str();
}

// The board's hardware serial (from /proc/cpuinfo) rather than a fixed
// configured name, so multiple aipicam units are distinguishable in a
// phone's WiFi network list (the AP's own SSID -- see ap_control.hpp)
// instead of all showing the same name. Falls back to
// wifi.device_name (e.g. when not running on real Pi hardware) if it
// can't be read.
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

// Waits long enough for the just-sent HTTP response to actually reach
// the client before anything this triggers (an AP teardown, in
// particular) could drop the connection it travelled over, then
// reboots. Fire-and-forget: once the reboot command is issued, the
// whole system (including this process) is going down regardless of
// what run_command reports back.
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
    const std::string configured_name = cfg.get_str("wifi.device_name", "pi-bluetooth-configuration");
    const std::string serial     = read_pi_serial();
    const std::string dev_name   = serial.empty() ? configured_name : serial;
    const std::string iface      = cfg.get_str("wifi.interface", "wlan0");
    const std::string eth_iface  = cfg.get_str("ethernet.interface", "eth0");
    const std::string eth_default_ip = cfg.get_str("ethernet.ip", "192.168.4.1");
    const int eth_default_range_start = cfg.get_int("ethernet.dhcp_range_start", 2);
    const int eth_default_range_end   = cfg.get_int("ethernet.dhcp_range_end", 200);
    const int sta_boot_timeout_secs = cfg.get_int("wifi.connect_timeout_secs", 20);
    const int max_scan_results   = cfg.get_int("scan.max_results", 10);
    const auto relays = relayctl::load_relays(cfg_path);

    // One mutex per configured relay port -- populated once, up front,
    // before any thread that might read it starts, so every later
    // relay_port_mu[port] lookup below only ever finds an existing key
    // and never triggers a concurrent std::map insert/rehash. Keyed per
    // port (not one mutex for every relay) so a request against one
    // relay is never blocked behind a slow or stuck query against a
    // completely different one -- see relays_json/do_relay for why a
    // lock is needed here at all.
    std::map<int, std::mutex> relay_port_mu;
    for (const auto& r : relays) relay_port_mu[r.port];
    const int victron_ctrl_port = cfg.get_int("victron.ctrl_port", 8562);

    // The AP's own subnet -- deliberately distinct from eth0's default
    // gateway (192.168.4.1) so the two can never collide if a client
    // happens to be on both eth0 and the AP at once (unusual, but not
    // impossible: a laptop with both a USB-C dock and its own WiFi, say).
    const std::string ap_ip = cfg.get_str("ap.ip", "192.168.5.1");
    const int ap_range_start = cfg.get_int("ap.dhcp_range_start", 2);
    const int ap_range_end   = cfg.get_int("ap.dhcp_range_end", 200);
    const int http_port      = cfg.get_int("http.port", 8080);

    std::cerr << "[Config] device   : " << dev_name << (serial.empty() ? " (configured)" : " (hardware serial)") << "\n"
              << "[Config] wifi if  : " << iface << "\n"
              << "[Config] eth if   : " << eth_iface << "\n"
              << "[Config] ap ip    : " << ap_ip << "\n"
              << "[Config] http port: " << http_port << "\n"
              << "[Config] relays   : " << relays.size() << " configured\n"
              << "[Config] victron  : ctrl_port " << victron_ctrl_port << "\n";

    WifiControl wifi(iface);
    ethctl::EthControl eth(eth_iface);
    apctl::ApControl ap(iface, ap_ip, ap_range_start, ap_range_end);

    // Advertised as soon as possible, independent of WiFi's own
    // station-vs-AP boot sequence below -- it adapts to whichever
    // interfaces/addresses actually come and go on its own (see
    // mdns_responder.hpp's periodic refresh), so there's no need to
    // sequence this after that decision is made. Lets the client app
    // find this Pi automatically (Bonjour/NWBrowser on iOS) instead of
    // requiring its address to be typed in.
    mdns::MdnsResponder mdns_responder(dev_name, static_cast<uint16_t>(http_port));
    {
        std::string mdns_err;
        if (!mdns_responder.start(mdns_err)) {
            std::cerr << "[mDNS] failed to start: " << mdns_err << " -- the app will need the Pi's address entered manually\n";
        } else {
            std::cerr << "[mDNS] advertising " << dev_name << "." << mdns::SERVICE_TYPE << "\n";
        }
    }

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

    // Tries to join whatever's already configured -- wpa_supplicant,
    // already started by OpenRC before this daemon (see its own
    // depend()), attempts this entirely on its own; this just waits to
    // see whether it succeeds within a bounded time. Covers both "wrong
    // password/network out of range" and "nothing configured at all yet"
    // the same way: either one just fails to reach CONNECTED before the
    // timeout, falling through to AP mode below.
    bool sta_ok = false;
    for (int i = 0; i < sta_boot_timeout_secs * 2; ++i) {
        if (wifi.get_status().state == WifiStatus::CONNECTED) { sta_ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (sta_ok) {
        std::cerr << "[Wifi] joined " << wifi.get_status().ssid << " on boot\n";
    } else {
        std::cerr << "[Wifi] no configured network joined within " << sta_boot_timeout_secs
                   << "s -- starting AP mode\n";
        std::string ap_err;
        if (!ap.start(dev_name, ap_err)) {
            std::cerr << "[AP] failed to start: " << ap_err << "\n";
        }
    }

    httpsrv::HttpServer server;

    std::mutex scan_mu;
    std::string last_scan_json = "[]";

    auto do_scan = [&]() {
        auto results = wifi.scan(max_scan_results);
        std::lock_guard<std::mutex> lk(scan_mu);
        last_scan_json = scan_json(results);
    };

    // The one action with genuinely different behavior depending on
    // whether AP mode is currently active, because of a hard hardware
    // constraint: this radio can't run AP and station mode at once, so
    // submitting real credentials while AP mode is active necessarily
    // means the phone's own connection to this daemon (reached via the
    // AP) is about to be severed the moment wlan0 switches over --
    // there's no way to keep serving that same phone a "did it work"
    // answer afterward. So in that case, this treats a successful join
    // as automatically finished too (marking MARKER_FILE and rebooting
    // immediately, same as POST /finish normally would) rather than
    // waiting for a separate finish step nothing could ever reach; and
    // on failure, it restarts the AP so the phone (which will have
    // noticed its connection drop either way) has something to
    // reconnect to and retry against.
    //
    // If AP mode was NOT active (this Pi is already on a real network --
    // wlan0 stays in station mode throughout, nothing about the phone's
    // own connection to this daemon changes), this behaves exactly like
    // the old BLE-era flow: joins the network, but leaves marking
    // MARKER_FILE/rebooting to a separate POST /finish, so local network
    // (Ethernet) settings can still be adjusted first if needed.
    auto do_connect = [&](const std::string& ssid, const std::string& psk) {
        bool was_ap = ap.is_running();
        if (was_ap) {
            std::string ap_err;
            if (!ap.stop(ap_err)) std::cerr << "[AP] failed to stop cleanly: " << ap_err << "\n";
        }

        bool ok = wifi.connect(ssid, psk);

        if (!was_ap) return; // old, unchanged behavior -- see comment above

        if (ok) {
            std::ofstream(MARKER_FILE).close();
            std::cerr << "[Wifi] joined " << ssid << " from AP mode -- finishing and rebooting\n";
            reboot_after_delay();
        } else {
            std::cerr << "[Wifi] failed to join " << ssid << " from AP mode -- restarting AP\n";
            std::string ap_err;
            if (!ap.start(dev_name, ap_err)) std::cerr << "[AP] failed to restart: " << ap_err << "\n";
        }
    };

    auto do_forget = [&]() {
        wifi.forget();
        std::remove(MARKER_FILE);
        reboot_after_delay();
    };

    // The end of the wizard when it didn't start from AP mode (see
    // do_connect) -- only meaningful once WiFi is actually connected,
    // since finishing before that would reboot into a Pi that isn't
    // actually configured. Marks success on disk and reboots into the
    // Pi's normal role rather than staying up to be managed further.
    auto do_finish = [&]() {
        if (wifi.get_status().state != WifiStatus::CONNECTED) {
            std::cerr << "[Finish] ignoring: WiFi is not connected\n";
            return;
        }
        std::ofstream(MARKER_FILE).close();
        reboot_after_delay();
    };

    // Ethernet direct-connect is only reconfigurable until the wizard
    // finishes (MARKER_FILE doesn't exist yet) -- that includes the
    // whole time AP mode is active, and the window after WiFi connects
    // but before "finish"/an AP-mode connect concludes things. Once
    // finished, eth0's gateway config is left as-is and the app switches
    // to a read-only display of it. Applied immediately when allowed, no
    // reboot needed (see file header comment).
    auto do_set_ethernet = [&](const std::string& ip, int range_start, int range_end) {
        if (marker_exists(MARKER_FILE)) {
            std::cerr << "[Ethernet] ignoring set_ethernet: setup has already finished\n";
            return;
        }
        std::string ip_err;
        if (!eth.set_static_ip(ip, range_start, range_end, ip_err)) {
            std::cerr << "[Ethernet] failed to set static IP " << ip << ": " << ip_err << "\n";
        }
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
    // Forwards on/off to whichever relay pi-relay-control-alpine has
    // listening on that TCP port (see relay_control.hpp) and returns the
    // refreshed relay list either way -- including on failure (state
    // comes back "unknown"), so the response reflects reality rather
    // than optimistically assuming the write worked.
    auto do_relay = [&](int port, const std::string& action) -> bool {
        if (!marker_exists(MARKER_FILE)) {
            std::cerr << "[Relay] ignoring relay command: setup has not finished yet\n";
            return false;
        }
        // relay_port_mu is pre-populated once at startup with exactly the
        // configured ports (see its declaration above) so every lookup
        // below is a plain read on an existing key, never a concurrent
        // std::map insert racing another request's own lookup. port
        // comes straight from the client's request, unvalidated, so that
        // guarantee only holds if an unconfigured port is rejected here
        // first, before ever touching the map.
        bool configured = std::any_of(relays.begin(), relays.end(),
                                       [&](const relayctl::RelayConfig& r) { return r.port == port; });
        if (!configured) {
            std::cerr << "[Relay] ignoring relay command for unconfigured port " << port << "\n";
            return false;
        }
        // Retries until pi-relay-control-alpine actually confirms the
        // change ("OK RELAY=ON"/"OK RELAY=OFF") rather than accepting the
        // first attempt regardless of outcome -- a transient failure
        // (relay busy, a dropped connection, etc.) would otherwise
        // silently leave the relay unchanged with only a log line to
        // show for it. Bounded by both an attempt count and a wall-clock
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
        return confirmed;
    };

    server.route("GET", "/status", [&](const httpsrv::Request&) {
        std::string relays_str = marker_exists(MARKER_FILE) ? relays_json(relays, relay_port_mu) : "[]";
        std::string scan_str;
        {
            std::lock_guard<std::mutex> lk(scan_mu);
            scan_str = last_scan_json;
        }
        std::ostringstream o;
        o << "{\"wifi\":" << status_json(wifi.get_status(), marker_exists(MARKER_FILE)) << ","
          << "\"apActive\":" << (ap.is_running() ? "true" : "false") << ","
          << "\"eth\":" << eth_config_json(eth) << ","
          << "\"leases\":" << leases_json(eth.get_leases()) << ","
          << "\"relays\":" << relays_str << ","
          << "\"victron\":" << victron_json(victronctl::query_status(victron_ctrl_port)) << ","
          << "\"scan\":" << scan_str << "}";
        return httpsrv::Response::json(o.str());
    });

    server.route("POST", "/scan", [&](const httpsrv::Request&) {
        std::thread([&]() { InflightGuard guard; do_scan(); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/connect", [&](const httpsrv::Request& req) {
        std::string ssid = json_get_string(req.body, "ssid");
        std::string psk = json_get_string(req.body, "password");
        if (ssid.empty()) return httpsrv::Response::error(400, "ssid is required");
        std::cerr << "[Command] connect requested: \"" << ssid << "\"\n";
        std::thread([&, ssid, psk]() { InflightGuard guard; do_connect(ssid, psk); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/forget", [&](const httpsrv::Request&) {
        std::cerr << "[Command] forget requested\n";
        std::thread([&]() { InflightGuard guard; do_forget(); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/finish", [&](const httpsrv::Request&) {
        std::cerr << "[Command] finish requested\n";
        std::thread([&]() { InflightGuard guard; do_finish(); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("GET", "/ethernet", [&](const httpsrv::Request&) {
        return httpsrv::Response::json(eth_config_json(eth));
    });

    server.route("POST", "/ethernet", [&](const httpsrv::Request& req) {
        std::string ip = json_get_string(req.body, "ip");
        long range_start = json_get_int(req.body, "rangeStart", -1);
        long range_end = json_get_int(req.body, "rangeEnd", -1);
        if (ip.empty() || range_start < 0 || range_end < 0) {
            return httpsrv::Response::error(400, "ip, rangeStart and rangeEnd are required");
        }
        std::cerr << "[Command] set_ethernet requested: " << ip << "," << range_start << "," << range_end << "\n";
        std::thread([&, ip, range_start, range_end]() {
            InflightGuard guard;
            do_set_ethernet(ip, static_cast<int>(range_start), static_cast<int>(range_end));
        }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/relay", [&](const httpsrv::Request& req) {
        long port = json_get_int(req.body, "port", -1);
        std::string action = json_get_string(req.body, "state");
        if (port < 0 || (action != "on" && action != "off")) {
            return httpsrv::Response::error(400, "port and state (\"on\"|\"off\") are required");
        }
        std::cerr << "[Command] relay " << port << " " << action << " requested\n";
        bool ok = do_relay(static_cast<int>(port), action);
        std::ostringstream o;
        o << "{\"ok\":" << (ok ? "true" : "false") << ","
          << "\"relays\":" << relays_json(relays, relay_port_mu) << "}";
        return httpsrv::Response::json(o.str());
    });

    std::string http_err;
    if (!server.start(http_port, http_err)) {
        std::cerr << "[HTTP] failed to start: " << http_err << "\n";
        return 1;
    }
    std::cerr << "[HTTP] listening on :" << http_port << " as \"" << dev_name << "\"\n";

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // http_server.hpp runs its own accept-loop and per-connection threads
    // -- nothing here needs to keep pumping anything, just wait for a
    // shutdown signal.
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cerr << "[Main] shutting down, waiting for in-flight scan/connect work...\n";
    while (g_inflight.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    mdns_responder.stop();

    return 0;
}
