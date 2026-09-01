#pragma once
/**
 * eth_control.hpp -- gives eth0 a fixed static IP and runs a DHCP server
 * (dnsmasq) scoped strictly to eth0, so a laptop plugged directly into
 * the Pi's ethernet port gets an address automatically with no router
 * in the loop -- a direct-connect path for local access/config that's
 * independent of whatever the Pi's WiFi is doing.
 *
 * eth0 is meant to always be a working gateway: main.cpp reapplies the
 * static IP here on every startup, so a fresh (or just-rebooted) Pi is
 * reachable over Ethernet immediately, no app interaction required.
 * Once WiFi provisioning is finished, the app switches this from an
 * editable field to a read-only display and the daemon rejects further
 * changes (see main.cpp's marker-file gate) -- Ethernet's job at that
 * point is a fixed fallback/maintenance path, not something to
 * reconfigure on the fly.
 *
 * The address is assigned directly with `ip addr add`, not through
 * dhcpcd's own static-ip config -- dhcpcd only applies its config once
 * it sees carrier on the interface, which is wrong for a gateway
 * address that needs to already be there *before* anything is plugged
 * in, so a client is served the instant it connects. dhcpcd is told to
 * ignore eth0 entirely (`denyinterfaces`) so it can't fight over the
 * address once carrier does appear.
 *
 * Unlike WiFi, applying this doesn't need a reboot: Ethernet doesn't
 * share the Pi 3's antenna with Bluetooth, so there's no coexistence
 * problem to route around -- the affected services (dhcpcd, dnsmasq)
 * are just restarted directly and the change takes effect immediately.
 *
 * The chosen IP and DHCP range are persisted in a plain state file so
 * they survive reboots (unlike `ip addr add`, which doesn't); the
 * `denyinterfaces` line is persisted as a marker-delimited block inside
 * /etc/dhcpcd.conf so it can be added/removed idempotently without
 * disturbing whatever else is already in that file.
 *
 * Safety note: dnsmasq is configured with `interface=`/`bind-interfaces`
 * specifically so it only ever answers DHCP requests on eth0 -- getting
 * this wrong and having it serve the LAN/WiFi side too would hand out
 * conflicting addresses on a network this daemon doesn't own.
 */
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "subprocess.hpp"

namespace ethctl {

constexpr const char* DHCPCD_CONF  = "/etc/dhcpcd.conf";
constexpr const char* DNSMASQ_CONF = "/etc/dnsmasq.conf";
constexpr const char* LEASES_FILE  = "/var/lib/misc/dnsmasq.leases";
constexpr const char* STATE_FILE   = "/etc/pi-bluetooth-configuration/eth0-static-ip";
constexpr const char* BEGIN_MARKER = "# BEGIN pi-bluetooth-configuration eth0 static";
constexpr const char* END_MARKER   = "# END pi-bluetooth-configuration eth0 static";

struct Config {
    std::string ip;
    int range_start = 0;
    int range_end = 0;
};

struct Lease {
    std::string ip;
    std::string mac;
    std::string hostname; // "*" from dnsmasq means "unknown"
};

inline bool is_valid_ipv4(const std::string& ip) {
    std::stringstream ss(ip);
    std::string octet;
    int count = 0;
    while (std::getline(ss, octet, '.')) {
        if (octet.empty() || octet.size() > 3) return false;
        for (char c : octet) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        int v;
        try { v = std::stoi(octet); } catch (...) { return false; }
        if (v < 0 || v > 255) return false;
        ++count;
    }
    return count == 4;
}

// "192.168.4.1" -> "192.168.4" (the /24 network base).
inline std::string network_prefix24(const std::string& ip) {
    return ip.substr(0, ip.rfind('.'));
}

// "192.168.4.1" -> 1
inline int last_octet(const std::string& ip) {
    try { return std::stoi(ip.substr(ip.rfind('.') + 1)); } catch (...) { return -1; }
}

// Replaces (or removes entirely, if new_block is empty) our
// marker-delimited block in a config file, leaving everything else in
// the file untouched.
inline void replace_marker_block(const std::string& path, const std::string& new_block) {
    std::ifstream in(path);
    std::ostringstream kept;
    std::string line;
    bool in_block = false;
    while (std::getline(in, line)) {
        if (line == BEGIN_MARKER) { in_block = true; continue; }
        if (line == END_MARKER) { in_block = false; continue; }
        if (!in_block) kept << line << "\n";
    }
    in.close();

    std::ofstream out(path, std::ios::trunc);
    out << kept.str();
    if (!new_block.empty()) {
        out << BEGIN_MARKER << "\n" << new_block << END_MARKER << "\n";
    }
}

class EthControl {
public:
    explicit EthControl(std::string iface) : iface_(std::move(iface)) {}

    // Current live IP on the interface -- whatever's actually assigned
    // right now. Since the address is set directly via `ip addr add`,
    // this reflects reality regardless of carrier state.
    std::string get_ip() const {
        auto r = run_command({"ip", "-4", "-o", "addr", "show", "dev", iface_});
        auto pos = r.output.find(" inet ");
        if (pos == std::string::npos) return "";
        pos += 6;
        auto slash = r.output.find('/', pos);
        if (slash == std::string::npos) return "";
        std::string ip = r.output.substr(pos, slash - pos);
        const char* ws = " \t\r\n";
        auto a = ip.find_first_not_of(ws);
        if (a == std::string::npos) return "";
        auto b = ip.find_last_not_of(ws);
        return ip.substr(a, b - a + 1);
    }

    // Whatever was last persisted -- empty ip/zero range if nothing has
    // been chosen yet (fresh install, before the first ensure_static_ip
    // call has finished).
    Config get_config() const {
        Config c;
        std::ifstream in(STATE_FILE);
        std::string line;
        if (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string ip, start, end;
            std::getline(ss, ip, ',');
            std::getline(ss, start, ',');
            std::getline(ss, end, ',');
            c.ip = ip;
            try { c.range_start = std::stoi(start); } catch (...) {}
            try { c.range_end = std::stoi(end); } catch (...) {}
        }
        return c;
    }

    // Currently-allocated DHCP leases handed out to devices plugged
    // into eth0 -- parsed from dnsmasq's own leases file (one line per
    // active lease: "<expiry> <mac> <ip> <hostname> <client-id>").
    std::vector<Lease> get_leases() const {
        std::vector<Lease> leases;
        std::ifstream in(LEASES_FILE);
        std::string line;
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string expiry, mac, ip, hostname;
            if (!(ss >> expiry >> mac >> ip >> hostname)) continue;
            leases.push_back({ip, mac, hostname});
        }
        return leases;
    }

    bool set_static_ip(const std::string& ip, int range_start, int range_end, std::string& err) {
        if (!is_valid_ipv4(ip)) {
            err = "invalid IPv4 address";
            return false;
        }
        if (range_start < 1 || range_start > 254 || range_end < 1 || range_end > 254 || range_start > range_end) {
            err = "invalid DHCP range";
            return false;
        }
        if (last_octet(ip) >= range_start && last_octet(ip) <= range_end) {
            err = "DHCP range overlaps the gateway's own address";
            return false;
        }

        std::string prefix = network_prefix24(ip);

        {
            std::ofstream out(STATE_FILE, std::ios::trunc);
            out << ip << "," << range_start << "," << range_end << "\n";
        }

        // Keep dhcpcd from ever touching eth0 -- it only applies static
        // config once it sees carrier, which is wrong for an address
        // that needs to already be there before anything is plugged in.
        std::ostringstream dhcpcd_block;
        dhcpcd_block << "denyinterfaces " << iface_ << "\n";
        replace_marker_block(DHCPCD_CONF, dhcpcd_block.str());
        run_command({"rc-service", "dhcpcd", "restart"}, 20);

        // Assign the address ourselves, directly -- independent of
        // carrier, so it's already there the instant a cable is plugged in.
        run_command({"ip", "addr", "flush", "dev", iface_});
        auto add = run_command({"ip", "addr", "add", ip + "/24", "dev", iface_});
        if (add.exit_code != 0) {
            err = "failed to assign address: " + add.output;
            return false;
        }
        run_command({"ip", "link", "set", iface_, "up"});

        std::ostringstream dnsmasq_block;
        dnsmasq_block << "interface=" << iface_ << "\n"
                       << "bind-interfaces\n"
                       << "dhcp-authoritative\n"
                       << "port=0\n" // DHCP only -- no DNS service
                       << "dhcp-leasefile=" << LEASES_FILE << "\n"
                       << "dhcp-range=" << prefix << "." << range_start << ","
                       << prefix << "." << range_end << ",255.255.255.0,12h\n";
        replace_marker_block(DNSMASQ_CONF, dnsmasq_block.str());

        run_command({"rc-update", "add", "dnsmasq", "default"});
        run_command({"rc-service", "dnsmasq", "stop"});
        auto start = run_command({"rc-service", "dnsmasq", "start"}, 20);
        if (start.exit_code != 0) {
            err = "dnsmasq failed to start: " + start.output;
            return false;
        }
        return true;
    }

    // Reapplies whichever IP/range was last chosen (persisted state, or
    // the config defaults if nothing was ever chosen) -- called at
    // every daemon startup, since `ip addr add` doesn't survive a
    // reboot on its own.
    bool ensure_static_ip(const std::string& default_ip, int default_start, int default_end, std::string& err) {
        Config c = get_config();
        std::string ip = c.ip.empty() ? default_ip : c.ip;
        int start = c.range_start > 0 ? c.range_start : default_start;
        int end = c.range_end > 0 ? c.range_end : default_end;
        return set_static_ip(ip, start, end, err);
    }

private:
    std::string iface_;
};

} // namespace ethctl
