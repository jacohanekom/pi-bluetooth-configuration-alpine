#pragma once
/**
 * network_lock.hpp -- ap_control.hpp (wlan0's fallback AP), wifi_control
 * .hpp's connect() (station mode), and eth_control.hpp (the eth0/USB
 * Ethernet bridge gateway) all touch the same shared, system-wide
 * network stack: /etc/dnsmasq.conf and the single dnsmasq process
 * serving both wlan0 and the bridge, and -- confirmed on real hardware,
 * not just suspected -- dhcpcd itself, which every one of dnsmasq's and
 * hostapd's own OpenRC init scripts depends on via `need net` (dhcpcd
 * is this image's only `net` provider). Started here as a narrower
 * dnsmasq-only lock; broadened after a real device (a Pi Zero W with a
 * USB Ethernet bridge) hit "cannot start dnsmasq/hostapd as dhcpcd
 * would not start" and "associated but no IPv4 address was assigned"
 * simultaneously -- POST /ethernet's do_set_ethernet (restarting dhcpcd
 * for the bridge) and do_finish()'s live-join thread (tearing down the
 * AP, then wifi.connect()'s own dhcpcd invocation for wlan0) had run
 * fully concurrently, with nothing serializing them: dhcpcd mid-restart
 * from one thread is exactly what makes OpenRC refuse to (re)start it
 * as a dependency from the other, and refuses a clean lease request on
 * wlan0 at the same time.
 *
 * One coarse-grained mutex, held for the ENTIRE body of each of these
 * functions (not just their dnsmasq-touching lines) -- dhcpcd, dnsmasq,
 * and hostapd are all one shared, mutually-exclusive piece of state on
 * this device, not independent resources that happen to share a file.
 * Held across a function that can itself take up to ~25s
 * (wifi_control.hpp's connect(), between association and the dhcpcd
 * lease timeout) is a deliberate trade-off: a concurrent Ethernet
 * reconfiguration during that window should wait, not race.
 */
#include <mutex>

namespace network_guard {
inline std::mutex mu;
}
