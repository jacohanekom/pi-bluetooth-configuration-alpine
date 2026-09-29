#pragma once
/**
 * dnsmasq_lock.hpp -- ap_control.hpp (wlan0's fallback AP) and
 * eth_control.hpp (the eth0/bridge gateway) both read-modify-write the
 * same /etc/dnsmasq.conf (via replace_marker_block, each scoped to its
 * own marker-delimited block) and both stop/start or restart the single
 * shared dnsmasq service afterward -- see ap_control.hpp's own header
 * comment on why one dnsmasq process serving multiple interfaces is
 * intentional, not accidental.
 *
 * Without a shared lock, two threads doing this concurrently is a real
 * failure mode, not just a theoretical one: main.cpp's own boot-time
 * Ethernet-bridge setup thread (eth.ensure_static_ip(), backgrounded so
 * a slow-to-enumerate USB Ethernet bridge doesn't hold up boot) and
 * do_finish()'s live-join thread (ap.stop(), triggered whenever the
 * setup wizard finishes) can genuinely overlap on real hardware --
 * confirmed on a Pi Zero W with a USB Ethernet bridge, where USB
 * enumeration/bridging is slow enough on a single ARMv6 core that the
 * boot-time thread was still running when the wizard finished only
 * moments later. Racing on the file itself can lose one thread's edit
 * outright (a classic read-modify-write race); racing on the service
 * itself (one thread's `stop`/`start` overlapping the other's
 * `restart`) can make either one fail outright, depending on
 * supervise-daemon's own state at that exact moment -- exactly the
 * intermittent "dnsmasq failed to start"/"failed to set static IP"
 * failures seen on that device.
 */
#include <mutex>

namespace dnsmasq_guard {
inline std::mutex mu;
}
