#pragma once
/**
 * pico_transport.hpp -- BLE peripheral role hosted entirely on a
 * Raspberry Pi Pico 2 W (its own BTstack-based GATT server, see
 * pico/aipicam_ble_bridge/aipicam_ble_bridge.ino), connected to this Pi
 * over USB as a plain CDC-ACM serial port. Replaces main.cpp's previous
 * direct BlueZ/D-Bus integration (gatt_server.hpp, agent.hpp -- both
 * removed) entirely.
 *
 * Why: BlueZ's own built-in GATT profiles (Battery Service, Device
 * Information, Current Time Service -- compiled directly into this
 * Alpine build's bluetoothd, not anything this daemon ever registered
 * itself) declare characteristics that demand an authenticated link.
 * iOS's system Bluetooth daemon reads Battery Level on every connected
 * peripheral automatically, for its own "device battery %" indicator,
 * independent of anything this app's own client does. That read fails
 * with Insufficient Authentication, BlueZ sends an SMP Security Request
 * to fix that, and -- confirmed via a live HCI trace (btmon) against a
 * real device -- pairing fails and BlueZ disconnects the device outright
 * a few seconds later, every single connection, forever. This persisted
 * even after disabling BlueZ's "battery" plugin (this specific Alpine
 * bluez build apparently compiles these profiles in directly rather than
 * as a runtime-disableable plugin) and after registering a
 * NoInputNoOutput pairing agent to let that authentication complete via
 * headless Just Works bonding. With two targeted fixes both proving
 * insufficient, and switching the underlying Bluetooth adapter itself
 * (onboard Broadcom vs. an external Realtek USB dongle) making no
 * difference either, the most likely explanation is something baked into
 * this specific Alpine bluez package build rather than anything fixable
 * from userspace config. Offloading the BLE peripheral role to a Pico's
 * own, completely independent BTstack build sidesteps that entirely,
 * rather than continuing to fight it.
 *
 * Design: main.cpp's actual WiFi/relay/Ethernet business logic is
 * untouched -- it only ever calls add_characteristic()/notify()/start()
 * on whatever server object it's given, and those three keep the exact
 * same shape gatt_server.hpp's GattServer had. The Pico firmware
 * hardcodes the same nine UUIDs this daemon has always used (see
 * main.cpp's own *_UUID constants and the firmware's matching header
 * comment) against short mnemonic names -- there's nothing to negotiate
 * over the wire beyond characteristic values themselves.
 *
 * Wire protocol (newline-delimited ASCII, values base64-encoded so
 * arbitrary bytes -- including embedded newlines, though none of this
 * project's characteristics ever actually contain one -- can't desync
 * the line-based framing):
 *
 *   Pico -> Pi:  READY
 *     Sent once at boot, after BLE advertising has actually started.
 *     start() below blocks (up to 15s) for this before seeding initial
 *     characteristic values, so they're not sent into a not-yet-open
 *     serial read on the Pico's side right after a reset/reflash.
 *
 *   Pi -> Pico:  VALUE <name> <base64>
 *     Pushes a fresh value for characteristic <name>. The Pico updates
 *     its own local cache (serving future BLE reads instantly, no
 *     serial round trip at read time) and, if that characteristic has
 *     NOTIFY and a central is currently subscribed, sends a BLE
 *     notification with it immediately.
 *
 *   Pico -> Pi:  WRITE <name> <base64>
 *     A connected central wrote to characteristic <name>; forwarded
 *     verbatim to that characteristic's on_write callback.
 *
 * <name> is one of: SSID, PSK, COMMAND, STATUS, SCAN, ETH, LEASES,
 * RELAYS, VICTRON -- see uuid_to_name() below for the exact mapping.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace picoserv {

namespace detail {

inline std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += tbl[(n >> 18) & 0x3f];
        out += tbl[(n >> 12) & 0x3f];
        out += tbl[(n >> 6) & 0x3f];
        out += tbl[n & 0x3f];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out += tbl[(n >> 18) & 0x3f];
        out += tbl[(n >> 12) & 0x3f];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += tbl[(n >> 18) & 0x3f];
        out += tbl[(n >> 12) & 0x3f];
        out += tbl[(n >> 6) & 0x3f];
        out += "=";
    }
    return out;
}

inline int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::vector<uint8_t> base64_decode(const std::string& in) {
    std::vector<uint8_t> out;
    int vals[4];
    int vi = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        int v = b64_val(c);
        if (v < 0) continue;
        vals[vi++] = v;
        if (vi == 4) {
            uint32_t n = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12) |
                         (static_cast<uint32_t>(vals[2]) << 6) | static_cast<uint32_t>(vals[3]);
            out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
            out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
            out.push_back(static_cast<uint8_t>(n & 0xff));
            vi = 0;
        }
    }
    if (vi == 2) {
        uint32_t n = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12);
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    } else if (vi == 3) {
        uint32_t n = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12) |
                     (static_cast<uint32_t>(vals[2]) << 6);
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    }
    return out;
}

// Splits on whitespace, but only for the first (max_parts - 1) runs --
// the final part (a base64 payload) is returned whole even if it somehow
// contained whitespace itself (it shouldn't, but there's no reason to
// let that corrupt framing rather than just fail a decode later).
inline std::vector<std::string> split_ws(const std::string& s, int max_parts) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (static_cast<int>(parts.size()) < max_parts - 1) {
        size_t start = s.find_first_not_of(" \t", pos);
        if (start == std::string::npos) return parts;
        size_t end = s.find_first_of(" \t", start);
        if (end == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, end - start));
        pos = end;
    }
    size_t rest = s.find_first_not_of(" \t", pos);
    if (rest != std::string::npos) parts.push_back(s.substr(rest));
    return parts;
}

// The Pico firmware hardcodes this exact same table (UUID <-> short
// name) -- see its own header comment. Keeping add_characteristic()'s
// signature identical to gatt_server.hpp's (uuid first, not a name)
// means main.cpp's nine existing call sites needed zero changes beyond
// swapping which header/class they construct against.
inline std::string uuid_to_name(const std::string& uuid) {
    static const std::map<std::string, std::string> table = {
        {"7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10", "SSID"},
        {"7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10", "PSK"},
        {"7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10", "COMMAND"},
        {"7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10", "STATUS"},
        {"7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10", "SCAN"},
        {"7b1e0006-6a45-4d1f-9b0a-3c2f8e4d5a10", "ETH"},
        {"7b1e0007-6a45-4d1f-9b0a-3c2f8e4d5a10", "LEASES"},
        {"7b1e0008-6a45-4d1f-9b0a-3c2f8e4d5a10", "RELAYS"},
        {"7b1e0009-6a45-4d1f-9b0a-3c2f8e4d5a10", "VICTRON"},
    };
    auto it = table.find(uuid);
    return it != table.end() ? it->second : uuid;
}

} // namespace detail

struct Characteristic {
    std::string name;
    std::function<std::vector<uint8_t>()> on_read;
    std::function<void(const std::vector<uint8_t>&)> on_write;
};

class PicoTransport {
public:
    explicit PicoTransport(std::string serial_port) : serial_port_(std::move(serial_port)) {}

    ~PicoTransport() {
        running_ = false;
        if (reader_thread_.joinable()) reader_thread_.join();
        if (fd_ >= 0) close(fd_);
    }

    // flags is accepted only for signature parity with gatt_server.hpp's
    // GattServer -- READ-vs-not is already implied by on_read being
    // non-null, and NOTIFY-vs-not is baked into the Pico firmware's own
    // hardcoded table (see this file's header comment), so there's
    // nothing left for it to configure here.
    Characteristic* add_characteristic(const std::string& uuid, std::vector<std::string> /*flags*/,
                                        std::function<std::vector<uint8_t>()> on_read,
                                        std::function<void(const std::vector<uint8_t>&)> on_write) {
        auto c = std::make_unique<Characteristic>();
        c->name = detail::uuid_to_name(uuid);
        c->on_read = std::move(on_read);
        c->on_write = std::move(on_write);
        Characteristic* raw = c.get();
        by_name_[c->name] = raw;
        chars_.push_back(std::move(c));
        return raw;
    }

    bool start(std::string& err_out) {
        fd_ = open(serial_port_.c_str(), O_RDWR | O_NOCTTY);
        if (fd_ < 0) {
            err_out = "open(" + serial_port_ + ") failed: " + strerror(errno);
            return false;
        }

        termios tty{};
        if (tcgetattr(fd_, &tty) != 0) {
            err_out = std::string("tcgetattr failed: ") + strerror(errno);
            return false;
        }
        cfmakeraw(&tty);
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 0;
        // Meaningless to a USB-CDC-ACM device (there's no real UART
        // clock on the wire, just USB framing) but termios still
        // requires *a* value here.
        cfsetispeed(&tty, B115200);
        cfsetospeed(&tty, B115200);
        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            err_out = std::string("tcsetattr failed: ") + strerror(errno);
            return false;
        }

        running_ = true;
        reader_thread_ = std::thread([this]() { reader_loop(); });

        {
            std::unique_lock<std::mutex> lk(ready_mu_);
            ready_cv_.wait_for(lk, std::chrono::seconds(15), [this] { return ready_.load(); });
        }
        if (!ready_) {
            std::cerr << "[Pico] no READY within 15s of opening " << serial_port_
                       << " -- proceeding anyway, but it may not be listening yet\n";
        }

        // Seeds every characteristic's initial value -- without this, a
        // central reading one before its first natural notify-on-change
        // tick (5s later, for most of these -- see main.cpp's periodic
        // poll threads) would see the Pico's blank startup default
        // instead of a real reading.
        for (auto& c : chars_) {
            if (c->on_read) push_value(c->name, c->on_read());
        }

        return true;
    }

    void notify(Characteristic* chr, const std::vector<uint8_t>& value) { push_value(chr->name, value); }

private:
    void push_value(const std::string& name, const std::vector<uint8_t>& value) {
        std::string line = "VALUE " + name + " " + detail::base64_encode(value) + "\n";
        std::lock_guard<std::mutex> lk(write_mu_);
        // Best-effort: a dropped push self-heals on the next periodic
        // notify (see main.cpp's poll threads), the same tolerance this
        // daemon already had for an occasionally-dropped BLE notification
        // over the old BlueZ path.
        ssize_t n = write(fd_, line.data(), line.size());
        (void)n;
    }

    void reader_loop() {
        std::string buf;
        char chunk[256];
        while (running_) {
            ssize_t n = read(fd_, chunk, sizeof(chunk));
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            buf.append(chunk, static_cast<size_t>(n));
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                handle_line(line);
            }
        }
    }

    void handle_line(const std::string& line) {
        if (line == "READY") {
            {
                std::lock_guard<std::mutex> lk(ready_mu_);
                ready_ = true;
            }
            ready_cv_.notify_all();
            return;
        }

        auto parts = detail::split_ws(line, 3);
        if (parts.size() != 3 || parts[0] != "WRITE") return;
        auto it = by_name_.find(parts[1]);
        if (it == by_name_.end() || !it->second->on_write) return;
        it->second->on_write(detail::base64_decode(parts[2]));
    }

    std::string serial_port_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    std::mutex write_mu_;
    std::mutex ready_mu_;
    std::condition_variable ready_cv_;
    std::atomic<bool> ready_{false};
    std::vector<std::unique_ptr<Characteristic>> chars_;
    std::map<std::string, Characteristic*> by_name_;
};

} // namespace picoserv
