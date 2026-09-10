#pragma once
/**
 * eth_control.hpp -- gives eth0 (and, if present, a second wired
 * interface -- see below) a fixed static IP and runs a DHCP+DNS server
 * (dnsmasq) scoped strictly to them, so a laptop plugged directly into
 * either of the Pi's ethernet ports gets an address (and working DNS)
 * automatically with no router in the loop -- a direct-connect path for
 * local access/config that's independent of whatever the Pi's WiFi is
 * doing.
 *
 * Two wired interfaces, one network: when a second interface is
 * configured (`ethernet.interface2` in config.ini -- e.g. a USB-Ethernet
 * dongle showing up as eth1 alongside the onboard eth0), both are
 * bridged together into one Linux bridge device (BRIDGE_NAME) rather
 * than each getting its own address. This is the only correct way to
 * put two physical ports on the same logical network: giving eth0 and
 * eth1 the *same* IP directly isn't possible (two interfaces can't hold
 * one address without conflict), and giving them separate addresses in
 * the same subnet without a bridge would leave a device on eth0 with no
 * actual path to one on eth1 -- they'd be on the same numeric subnet but
 * different physical/broadcast domains, which Linux doesn't
 * automatically bridge just because the addresses happen to overlap.
 * The bridge makes both ports one real L2 segment, so the static IP,
 * DHCP scope, and DNS below are all applied to the bridge, not to either
 * physical interface. If no second interface is configured (or it's
 * configured but not actually present on this particular Pi --
 * interface2_exists() checks for real, so a config setting alone can't
 * make a missing dongle start working), the bridge still exists with
 * eth0 as its only member -- one code path either way.
 *
 * eth0 (and the bridge, once a second interface is involved) is meant to
 * always be a working gateway: main.cpp reapplies the static IP here on
 * every startup, so a fresh (or just-rebooted) Pi is reachable over
 * Ethernet immediately, no app interaction required. Once WiFi
 * provisioning is finished, the app switches this from an editable field
 * to a read-only display and the daemon rejects further changes (see
 * main.cpp's marker-file gate) -- Ethernet's job at that point is a
 * fixed fallback/maintenance path, not something to reconfigure on the
 * fly.
 *
 * The address is assigned directly with `ip addr add`, not through
 * dhcpcd's own static-ip config -- dhcpcd only applies its config once
 * it sees carrier on the interface, which is wrong for a gateway
 * address that needs to already be there *before* anything is plugged
 * in, so a client is served the instant it connects. dhcpcd is told to
 * ignore eth0/eth1/the bridge entirely (`denyinterfaces`) so it can't
 * fight over the address once carrier does appear.
 *
 * Unlike a WiFi network change, applying this doesn't need a reboot:
 * eth0/eth1/the bridge are entirely independent of whatever wlan0 is
 * doing (station mode, AP fallback, or mid-transition between the two --
 * see ap_control.hpp), so there's no coexistence problem to route around
 * -- the affected services (dhcpcd, dnsmasq) are just restarted directly
 * and the change takes effect immediately. WiFi's own connect/forget/
 * finish flows reboot by deliberate design choice (see main.cpp), not
 * because of any hardware necessity -- a working connection to reach
 * this daemon on again is only guaranteed after a full restart anyway,
 * once either flow concludes.
 *
 * The chosen IP and DHCP range are persisted in a plain state file so
 * they survive reboots (unlike `ip addr add`, which doesn't); the
 * `denyinterfaces` line is persisted as a marker-delimited block inside
 * /etc/dhcpcd.conf so it can be added/removed idempotently without
 * disturbing whatever else is already in that file.
 *
 * Safety note: dnsmasq is configured with `interface=`/`bind-interfaces`
 * specifically so it only ever answers DHCP *and DNS* requests on the
 * bridge -- getting this wrong and having it serve the LAN/WiFi side too
 * would hand out conflicting addresses (DHCP) or expose an open resolver
 * (DNS) on a network this daemon doesn't own. DNS queries from clients
 * are forwarded upstream using whatever nameservers are in
 * /etc/resolv.conf -- normally whatever WiFi's own DHCP handed dhcpcd --
 * so eth0/eth1 clients get the same DNS resolution WiFi clients on this
 * Pi's own upstream network would.
 *
 * enable_internet_sharing() NATs the bridge's traffic out through the
 * WiFi interface (which is what actually has the internet connection),
 * so a laptop plugged into either wired port gets real internet access
 * through the Pi, not just a link to the Pi itself. Applied once at
 * startup, independent of WiFi's own connection state -- the iptables
 * rules reference the WiFi interface by name and work whether or not
 * it's associated yet.
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
// Bridges eth0 with a second wired interface (if configured and
// present) into one local network -- see this file's own header
// comment for why a bridge, not separate addresses, is required for
// that. Linux network interface names are hard-capped at 15 characters
// (the kernel's IFNAMSIZ limit) -- confirmed live: an earlier, longer
// name here made `ip link add` fail silently (its exit code went
// unchecked), which cascaded into every subsequent bridge-dependent
// step failing with confusing, once-removed errors ("Cannot find
// device ..."). "br-lan" is short enough to leave real margin under
// that limit while still being distinct from any bridge a user might
// set up themselves for other purposes.
constexpr const char* BRIDGE_NAME  = "br-lan";

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

// Replaces (or removes entirely, if new_block is empty) a
// marker-delimited block in a config file, leaving everything else in
// the file untouched. begin/end default to this file's own eth0 markers;
// ap_control.hpp reuses this same helper with its own distinct markers
// so both can coexist in the same dnsmasq.conf, each managing only its
// own interface's block.
inline void replace_marker_block(const std::string& path, const std::string& new_block,
                                  const std::string& begin = BEGIN_MARKER, const std::string& end = END_MARKER) {
    std::ifstream in(path);
    std::ostringstream kept;
    std::string line;
    bool in_block = false;
    while (std::getline(in, line)) {
        if (line == begin) { in_block = true; continue; }
        if (line == end) { in_block = false; continue; }
        if (!in_block) kept << line << "\n";
    }
    in.close();

    std::ofstream out(path, std::ios::trunc);
    out << kept.str();
    if (!new_block.empty()) {
        out << begin << "\n" << new_block << end << "\n";
    }
}

// True if a network interface by this name actually exists on this Pi
// right now -- not just configured in config.ini. A second interface
// (e.g. a USB-Ethernet dongle) might be named in config.ini but not
// physically present on a given unit, or not plugged in; this is what
// lets that be a graceful no-op (still bridges eth0 alone) rather than
// an error.
inline bool interface_exists(const std::string& name) {
    std::ifstream f("/sys/class/net/" + name + "/operstate");
    return f.good();
}

class EthControl {
public:
    // iface2 is optional (empty string if there's no second wired
    // interface to bridge in) -- see this file's own header comment.
    explicit EthControl(std::string iface, std::string iface2 = "")
        : iface_(std::move(iface)), iface2_(std::move(iface2)) {}

    // Current live IP on the bridge -- whatever's actually assigned
    // right now. Since the address is set directly via `ip addr add`,
    // this reflects reality regardless of carrier state.
    std::string get_ip() const {
        auto r = run_command({"ip", "-4", "-o", "addr", "show", "dev", BRIDGE_NAME});
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

        bool have_iface2 = !iface2_.empty() && interface_exists(iface2_);

        // Keep dhcpcd from ever touching any of these -- it only applies
        // static config once it sees carrier, which is wrong for an
        // address that needs to already be there before anything is
        // plugged in. Includes the bridge itself, and the second
        // interface if it's actually present, not just configured.
        std::ostringstream dhcpcd_block;
        dhcpcd_block << "denyinterfaces " << iface_;
        if (have_iface2) dhcpcd_block << " " << iface2_;
        dhcpcd_block << " " << BRIDGE_NAME << "\n";
        replace_marker_block(DHCPCD_CONF, dhcpcd_block.str());
        run_command({"rc-service", "dhcpcd", "restart"}, 20);

        // Bridge eth0 (and the second interface, if configured and
        // actually present) into one local network -- see this file's
        // own header comment for why a bridge, not separate addresses,
        // is required for that. `ip link add` on a bridge that already
        // exists fails harmlessly (EEXIST) -- this runs on every startup
        // (see ensure_static_ip), so it has to be idempotent rather than
        // erroring out on the second and subsequent boots. Checked
        // explicitly (unlike most other steps here) specifically because
        // a *silent* failure here previously cascaded into every
        // subsequent step failing with a confusing, once-removed error
        // ("Cannot find device ...") instead of the real cause.
        if (!interface_exists(BRIDGE_NAME)) {
            auto bridge_add = run_command({"ip", "link", "add", "name", BRIDGE_NAME, "type", "bridge"});
            if (bridge_add.exit_code != 0) {
                err = "failed to create bridge " + std::string(BRIDGE_NAME) + ": " + bridge_add.output;
                return false;
            }
        }
        // Linux bridges run Spanning Tree Protocol by default, which
        // puts every newly-enslaved port through a "listening"/
        // "learning" delay (15s each phase by default, so up to ~30s)
        // before it actually forwards any traffic at all -- including
        // DHCP broadcasts. STP exists to prevent loops across multiple
        // *interconnected* bridges/switches; with exactly one bridge and
        // two leaf interfaces here, there's no loop to protect against,
        // so this disables it entirely rather than just shortening the
        // delay -- a DHCP client shouldn't have to wait out a forwarding
        // delay at all for something this simple.
        run_command({"ip", "link", "set", BRIDGE_NAME, "type", "bridge", "stp_state", "0"});
        run_command({"ip", "link", "set", iface_, "up"});
        run_command({"ip", "link", "set", iface_, "master", BRIDGE_NAME});
        if (have_iface2) {
            run_command({"ip", "link", "set", iface2_, "up"});
            run_command({"ip", "link", "set", iface2_, "master", BRIDGE_NAME});
        }

        // Assign the address to the bridge itself, not to either
        // physical interface -- independent of carrier, so it's already
        // there the instant a cable is plugged into either port.
        run_command({"ip", "addr", "flush", "dev", BRIDGE_NAME});
        auto add = run_command({"ip", "addr", "add", ip + "/24", "dev", BRIDGE_NAME});
        if (add.exit_code != 0) {
            err = "failed to assign address: " + add.output;
            return false;
        }
        run_command({"ip", "link", "set", BRIDGE_NAME, "up"});

        // dnsmasq also answers DNS queries from clients here (no
        // "port=0"), forwarding them upstream using whatever nameservers
        // are in /etc/resolv.conf -- normally whatever WiFi's own DHCP
        // handed dhcpcd. Without this, clients get an address and a
        // route to the internet (see "Internet sharing") but no working
        // DNS: dnsmasq with DNS disabled either hands out no DNS server
        // at all, or (if one were hardcoded) points clients at something
        // not actually listening on port 53. "bind-interfaces" +
        // "interface=<bridge>" below keeps this DNS service scoped to
        // the bridge only, same as the DHCP side -- see the Safety note
        // above. dhcp-option 6 is set explicitly (rather than relying on
        // dnsmasq's own auto-fill) so the DHCP lease unambiguously points
        // clients at this address for DNS.
        std::ostringstream dnsmasq_block;
        dnsmasq_block << "interface=" << BRIDGE_NAME << "\n"
                       << "bind-interfaces\n"
                       << "dhcp-authoritative\n"
                       << "dhcp-leasefile=" << LEASES_FILE << "\n"
                       << "dhcp-range=" << prefix << "." << range_start << ","
                       << prefix << "." << range_end << ",255.255.255.0,12h\n"
                       << "dhcp-option=option:dns-server," << ip << "\n";
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

    // The interface that actually carries the local network's traffic --
    // always the bridge, regardless of whether a second interface ended
    // up joining it. This is what enable_internet_sharing() below should
    // be given as its lan_iface, not eth0 directly, once eth0 might not
    // be the only member.
    static std::string lan_interface() { return BRIDGE_NAME; }

private:
    std::string iface_;
    std::string iface2_;
};

// Enables IPv4 forwarding and NATs lan_iface's (eth0's) traffic out
// through wan_iface (the WiFi interface, which is what actually has the
// internet connection) -- so a laptop plugged directly into eth0 gets
// real internet access routed through the Pi's own WiFi uplink, not just
// a link to the Pi itself.
//
// Idempotent and safe to call on every startup: each rule is checked
// with `iptables -C` before being added with `-A`, rather than appended
// unconditionally, so repeated restarts don't pile up duplicate rules
// (which would still work, just messily -- and would never be cleaned
// back up).
//
// Persists net.ipv4.ip_forward via a sysctl.d drop-in for the next boot
// (Alpine's own `sysctl` OpenRC service applies /etc/sysctl.d/* at boot,
// before this daemon starts) and also writes it directly to
// /proc/sys/net/ipv4/ip_forward so it's live immediately, the same
// "reapply now, don't wait for the next boot" pattern set_static_ip uses
// for eth0's own address.
inline bool enable_internet_sharing(const std::string& lan_iface, const std::string& wan_iface, std::string& err) {
    {
        std::ofstream sysctl_conf("/etc/sysctl.d/30-pi-bluetooth-configuration.conf", std::ios::trunc);
        if (sysctl_conf.is_open()) sysctl_conf << "net.ipv4.ip_forward = 1\n";
    }
    {
        std::ofstream forward("/proc/sys/net/ipv4/ip_forward");
        if (!forward.is_open()) {
            err = "failed to open /proc/sys/net/ipv4/ip_forward";
            return false;
        }
        forward << "1";
    }

    // `iptables -C` exits 0 if the rule already exists, non-zero
    // otherwise -- used purely as a presence check, its own output is
    // discarded either way.
    auto ensure_rule = [](const std::vector<std::string>& check_argv,
                          const std::vector<std::string>& add_argv,
                          std::string& err_out) -> bool {
        if (run_command(check_argv).exit_code == 0) return true;
        auto add = run_command(add_argv);
        if (add.exit_code != 0) {
            err_out += (err_out.empty() ? "" : "; ") + std::string("iptables rule failed: ") + add.output;
            return false;
        }
        return true;
    };

    bool ok = true;
    ok = ensure_rule(
        {"iptables", "-t", "nat", "-C", "POSTROUTING", "-o", wan_iface, "-j", "MASQUERADE"},
        {"iptables", "-t", "nat", "-A", "POSTROUTING", "-o", wan_iface, "-j", "MASQUERADE"},
        err) && ok;
    ok = ensure_rule(
        {"iptables", "-C", "FORWARD", "-i", lan_iface, "-o", wan_iface, "-j", "ACCEPT"},
        {"iptables", "-A", "FORWARD", "-i", lan_iface, "-o", wan_iface, "-j", "ACCEPT"},
        err) && ok;
    ok = ensure_rule(
        {"iptables", "-C", "FORWARD", "-i", wan_iface, "-o", lan_iface, "-m", "state",
         "--state", "RELATED,ESTABLISHED", "-j", "ACCEPT"},
        {"iptables", "-A", "FORWARD", "-i", wan_iface, "-o", lan_iface, "-m", "state",
         "--state", "RELATED,ESTABLISHED", "-j", "ACCEPT"},
        err) && ok;

    return ok;
}

} // namespace ethctl
