#pragma once
/**
 * ap_control.hpp -- switches wlan0 into AP (access point) mode via
 * hostapd + a dedicated dnsmasq DHCP scope, so a phone can join this
 * Pi's own WiFi network directly to configure it -- the fallback path
 * for when the Pi can't join a real network on its own (nothing
 * configured yet, or the configured network is unreachable/wrong
 * password).
 *
 * wlan0 can only be in one mode at a time on this hardware -- station
 * (joined to a real network, driven by wpa_supplicant) or AP (hostapd),
 * never both concurrently. main.cpp's boot sequence tries station mode
 * first: wpa_supplicant, already started by OpenRC before this daemon
 * (see its own depend()), attempts to join whatever's saved in
 * wpa_supplicant.conf entirely on its own. AP mode here is only reached
 * if that doesn't result in an IP within the boot timeout.
 *
 * The AP itself is deliberately open (no password) -- this project's
 * security model (see the README) already treats the WiFi-configuration
 * flow as suitable for a trusted home/lab environment only, not a public
 * one; requiring a password just to reach the setup flow that hands out
 * the real network's password in the clear anyway wouldn't add
 * meaningful protection, just friction.
 *
 * Shares dnsmasq.conf with eth_control.hpp's own marker-delimited block
 * for eth0 -- one dnsmasq process happily serves DHCP on multiple
 * interfaces at once, each with its own dhcp-range, so both blocks
 * coexist in the same file without conflict (see eth_control.hpp's own
 * Safety note on why each block is scoped to exactly one interface).
 */
#include <fstream>
#include <sstream>
#include <string>

#include "eth_control.hpp" // reuses network_prefix24/replace_marker_block
#include "subprocess.hpp"

namespace apctl {

constexpr const char* HOSTAPD_CONF = "/etc/hostapd/hostapd.conf";
constexpr const char* DNSMASQ_CONF = "/etc/dnsmasq.conf";
constexpr const char* BEGIN_MARKER = "# BEGIN pi-bluetooth-configuration wlan0-ap";
constexpr const char* END_MARKER   = "# END pi-bluetooth-configuration wlan0-ap";

class ApControl {
public:
    ApControl(std::string iface, std::string ip, int range_start, int range_end)
        : iface_(std::move(iface)), ip_(std::move(ip)), range_start_(range_start), range_end_(range_end) {}

    // ssid is normally this Pi's hardware serial (same convention the
    // old BLE-advertised name used) so multiple units are distinguishable
    // in a phone's WiFi network list, same reasoning as before.
    bool start(const std::string& ssid, std::string& err) {
        // wpa_supplicant and hostapd can't both hold the radio -- get the
        // former out of the way first. Harmless if it wasn't running
        // (e.g. a fresh boot with nothing configured at all, where it
        // may have already given up and exited on its own).
        run_command({"rc-service", "wpa_supplicant", "stop"}, 15);

        run_command({"ip", "addr", "flush", "dev", iface_});
        auto add = run_command({"ip", "addr", "add", ip_ + "/24", "dev", iface_});
        if (add.exit_code != 0) {
            err = "failed to assign AP address: " + add.output;
            return false;
        }
        run_command({"ip", "link", "set", iface_, "up"});

        std::ostringstream hostapd_conf;
        hostapd_conf << "interface=" << iface_ << "\n"
                     << "driver=nl80211\n"
                     << "ssid=" << ssid << "\n"
                     << "hw_mode=g\n"
                     << "channel=6\n"
                     << "auth_algs=1\n"
                     << "wmm_enabled=0\n";
        {
            std::ofstream out(HOSTAPD_CONF, std::ios::trunc);
            if (!out.is_open()) {
                err = "failed to write " + std::string(HOSTAPD_CONF);
                return false;
            }
            out << hostapd_conf.str();
        }

        std::string prefix = ethctl::network_prefix24(ip_);
        std::ostringstream dnsmasq_block;
        dnsmasq_block << "interface=" << iface_ << "\n"
                      << "bind-interfaces\n"
                      << "dhcp-authoritative\n"
                      << "dhcp-range=" << prefix << "." << range_start_ << ","
                      << prefix << "." << range_end_ << ",255.255.255.0,1h\n"
                      << "dhcp-option=option:dns-server," << ip_ << "\n";
        ethctl::replace_marker_block(DNSMASQ_CONF, dnsmasq_block.str(), BEGIN_MARKER, END_MARKER);

        // Deliberately NOT `rc-update add`-ed into the default runlevel:
        // hostapd's lifecycle is entirely this daemon's own decision
        // (station mode first, AP only as a fallback -- see main.cpp's
        // boot sequence), never OpenRC's boot-time runlevel processing.
        // Leaving hostapd there would mean it starts automatically on
        // every *future* boot too, racing this daemon's own wlan0 setup
        // regardless of whether AP mode is actually needed that time.
        run_command({"rc-service", "hostapd", "stop"});
        auto hostapd_start = run_command({"rc-service", "hostapd", "start"}, 20);
        if (hostapd_start.exit_code != 0) {
            err = "hostapd failed to start: " + hostapd_start.output;
            return false;
        }

        run_command({"rc-update", "add", "dnsmasq", "default"});
        auto dnsmasq_restart = run_command({"rc-service", "dnsmasq", "restart"}, 20);
        if (dnsmasq_restart.exit_code != 0) {
            err = "dnsmasq failed to restart: " + dnsmasq_restart.output;
            return false;
        }

        running_ = true;
        return true;
    }

    // Tears the AP back down and hands wlan0 back to wpa_supplicant --
    // called right before attempting to join a newly-submitted network.
    bool stop(std::string& err) {
        run_command({"rc-service", "hostapd", "stop"}, 15);
        ethctl::replace_marker_block(DNSMASQ_CONF, "", BEGIN_MARKER, END_MARKER);
        run_command({"rc-service", "dnsmasq", "restart"}, 20);
        run_command({"ip", "addr", "flush", "dev", iface_});

        auto start = run_command({"rc-service", "wpa_supplicant", "start"}, 15);
        running_ = false;
        if (start.exit_code != 0) {
            err = "wpa_supplicant failed to restart: " + start.output;
            return false;
        }
        return true;
    }

    bool is_running() const { return running_; }

private:
    std::string iface_;
    std::string ip_;
    int range_start_;
    int range_end_;
    bool running_ = false;
};

} // namespace apctl
