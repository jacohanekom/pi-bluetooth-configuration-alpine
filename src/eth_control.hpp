#pragma once
/**
 * eth_control.hpp -- gives eth0 a fixed static IP and runs a DHCP server
 * (dnsmasq) scoped strictly to eth0, so a laptop plugged directly into
 * the Pi's ethernet port gets an address automatically with no router
 * in the loop -- a direct-connect path for local access/config that's
 * independent of whatever the Pi's WiFi is doing.
 *
 * Unlike WiFi, applying this doesn't need a reboot: Ethernet doesn't
 * share the Pi 3's antenna with Bluetooth, so there's no coexistence
 * problem to route around -- the affected services (dhcpcd, dnsmasq)
 * are just restarted directly and the change takes effect immediately.
 *
 * Persists as a marker-delimited block inside /etc/dhcpcd.conf and
 * /etc/dnsmasq.conf, so it can be added/removed idempotently without
 * disturbing whatever else is already in those files.
 *
 * Safety note: dnsmasq is configured with `interface=`/`bind-interfaces`
 * specifically so it only ever answers DHCP requests on eth0 -- getting
 * this wrong and having it serve the LAN/WiFi side too would hand out
 * conflicting addresses on a network this daemon doesn't own.
 */
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include "subprocess.hpp"

namespace ethctl {

constexpr const char* DHCPCD_CONF  = "/etc/dhcpcd.conf";
constexpr const char* DNSMASQ_CONF = "/etc/dnsmasq.conf";
constexpr const char* BEGIN_MARKER = "# BEGIN pi-bluetooth-configuration eth0 static";
constexpr const char* END_MARKER   = "# END pi-bluetooth-configuration eth0 static";

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

    // Current live IP on the interface, static or DHCP-assigned --
    // whatever's actually configured right now.
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

    bool set_static_ip(const std::string& ip, std::string& err) {
        if (!is_valid_ipv4(ip)) {
            err = "invalid IPv4 address";
            return false;
        }

        std::string prefix = network_prefix24(ip);

        std::ostringstream dhcpcd_block;
        dhcpcd_block << "interface " << iface_ << "\n"
                      << "static ip_address=" << ip << "/24\n";
        replace_marker_block(DHCPCD_CONF, dhcpcd_block.str());

        std::ostringstream dnsmasq_block;
        dnsmasq_block << "interface=" << iface_ << "\n"
                       << "bind-interfaces\n"
                       << "dhcp-authoritative\n"
                       << "port=0\n" // DHCP only -- no DNS service
                       << "dhcp-range=" << prefix << ".2," << prefix << ".200,255.255.255.0,12h\n";
        replace_marker_block(DNSMASQ_CONF, dnsmasq_block.str());

        run_command({"rc-service", "dhcpcd", "restart"}, 20);

        run_command({"rc-update", "add", "dnsmasq", "default"});
        run_command({"rc-service", "dnsmasq", "stop"});
        auto start = run_command({"rc-service", "dnsmasq", "start"}, 20);
        if (start.exit_code != 0) {
            err = "dnsmasq failed to start: " + start.output;
            return false;
        }
        return true;
    }

    void clear_static_ip() {
        replace_marker_block(DHCPCD_CONF, "");
        replace_marker_block(DNSMASQ_CONF, "");
        run_command({"rc-service", "dnsmasq", "stop"});
        run_command({"rc-update", "del", "dnsmasq", "default"});
        run_command({"rc-service", "dhcpcd", "restart"}, 20);
    }

private:
    std::string iface_;
};

} // namespace ethctl
