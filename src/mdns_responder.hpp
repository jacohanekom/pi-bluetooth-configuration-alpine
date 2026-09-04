#pragma once
/**
 * mdns_responder.hpp -- minimal, dependency-free multicast DNS (mDNS)
 * responder advertising this daemon's HTTP API as a discoverable
 * "_aipicam._tcp" service (RFC 6762/6763 -- the protocol Apple calls
 * Bonjour), so the client app can find a Pi on the network automatically
 * instead of requiring its address to be typed in.
 *
 * Hand-rolled over a plain UDP multicast socket, same "only what's
 * actually needed" philosophy as http_server.hpp -- Avahi (the obvious
 * off-the-shelf alternative on Linux) hard-depends on the `dbus` package
 * in Alpine (`avahi-openrc` requires it), which would reintroduce
 * exactly the extra daemon/service-management surface this project
 * spent real effort removing when BLE/BlueZ went away (see git history
 * and main.cpp's own header comment). This implementation only covers
 * what a client actually needs to discover and resolve this one fixed
 * service -- it is not a general mDNS stack, and this daemon never
 * itself browses or resolves *other* services on the network.
 *
 * Advertises (instance and hostname are both this Pi's own hardware
 * serial, same as the fallback AP's own SSID -- see ap_control.hpp --
 * so multiple aipicam units are distinguishable the same way):
 *   PTR   _aipicam._tcp.local.             -> <instance>._aipicam._tcp.local.
 *   SRV   <instance>._aipicam._tcp.local.  -> <instance>.local.:<port>
 *   TXT   <instance>._aipicam._tcp.local.  -> (empty)
 *   A     <instance>.local.                -> this Pi's current IPv4 address(es)
 *   PTR   _services._dns-sd._udp.local.    -> _aipicam._tcp.local. (RFC 6763 9,
 *                                              the standard "what services
 *                                              exist here" meta-query, so
 *                                              generic browsing tools work too)
 *
 * Answers queries for the above on demand and re-announces (unsolicited
 * responses) a few times after startup and whenever this Pi's own set of
 * IPv4 addresses changes (WiFi joining/leaving, AP mode starting/
 * stopping, Ethernet being plugged in), per RFC 6762 section 8.3, so a
 * client already browsing notices without needing to re-query. No
 * probing (RFC 6762 section 8.1): the advertised name is always this
 * Pi's own unique hardware serial, so a same-name conflict on the
 * network is already vanishingly unlikely without needing the extra
 * startup delay/complexity probing exists to guard against.
 *
 * Multi-homed aware (this Pi normally has both wlan0 and eth0 up at
 * once -- see eth_control.hpp): joins the multicast group on every
 * active interface, and replies to a query on the same interface it
 * arrived on (via IP_PKTINFO), the same technique real multi-homed mDNS
 * responders (including Avahi) use.
 *
 * Linux-only (IP_PKTINFO/ip_mreqn, both Linux-specific) -- consistent
 * with the rest of this codebase, which already assumes a Linux/Alpine
 * target throughout (ap_control.hpp's `ip`/`rc-service` calls,
 * subprocess.hpp's fork/exec model). The wire-format pieces (DnsWriter,
 * parse_name/parse_questions, build_full_response) have no such
 * dependency and were verified independently, on macOS, against Apple's
 * own `dns-sd` command-line tool (the same underlying mDNS stack iOS's
 * NWBrowser uses) before being wired into this Linux-specific socket
 * layer -- see this project's PR history for that verification.
 */
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace mdns {

constexpr const char* MDNS_ADDR = "224.0.0.251";
constexpr uint16_t MDNS_PORT = 5353;
constexpr const char* SERVICE_TYPE = "_aipicam._tcp.local.";
constexpr const char* DNSSD_META_QUERY = "_services._dns-sd._udp.local.";

constexpr uint16_t TYPE_A   = 1;
constexpr uint16_t TYPE_PTR = 12;
constexpr uint16_t TYPE_TXT = 16;
constexpr uint16_t TYPE_SRV = 33;
constexpr uint16_t TYPE_ANY = 255;
constexpr uint16_t CLASS_IN       = 0x0001; // shared record (PTR) -- no cache-flush bit, see RFC 6762 10.2
constexpr uint16_t CLASS_IN_FLUSH = 0x8001; // unique record (SRV/TXT/A) -- cache-flush bit set

// ---------------------------------------------------------------------
// Wire-format helpers -- pure functions, no sockets. Verified directly
// (a small standalone harness reusing exactly this code, no Linux-only
// pieces involved) against Apple's own `dns-sd` before being wired into
// the Linux socket layer below.

inline void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xff));
}
inline void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 24));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    buf.push_back(static_cast<uint8_t>(v & 0xff));
}

// Appends a fully-qualified DNS name ("foo.bar.local.") in RFC 1035
// label format, always uncompressed. An uncompressed name is fully
// valid on the wire -- every compliant resolver (including this file's
// own parser below) must support both forms -- so always writing this
// way trades a handful of extra bytes per packet (irrelevant at this
// packet size) for a much smaller, easier-to-verify implementation than
// also handling compression on the write side.
inline void write_name(std::vector<uint8_t>& buf, std::string name) {
    if (!name.empty() && name.back() == '.') name.pop_back();
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        std::string label = (dot == std::string::npos) ? name.substr(start) : name.substr(start, dot - start);
        buf.push_back(static_cast<uint8_t>(label.size()));
        for (char c : label) buf.push_back(static_cast<uint8_t>(c));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    buf.push_back(0);
}

// Parses a (possibly compressed) DNS name starting at data[offset],
// returning the offset just past it *in the original packet* (i.e. past
// a 2-byte pointer if one was followed, not into the pointed-to data --
// a pointer is always the last element of a name, RFC 1035 4.1.4).
// Bounds every read against len and caps the number of pointer hops
// followed, so a malformed or hostile packet can't run this off the end
// of the buffer or into an infinite pointer loop. Real queriers
// (including iOS's own mDNS stack) commonly send compressed names, so
// this side of the code has to handle them even though write_name above
// never produces them.
inline bool parse_name(const uint8_t* data, size_t len, size_t offset, std::string& out, size_t& end_offset) {
    out.clear();
    size_t pos = offset;
    bool jumped = false;
    size_t after_pointer = 0;
    int hops = 0;
    while (true) {
        if (pos >= len) return false;
        uint8_t b = data[pos];
        if ((b & 0xc0) == 0xc0) {
            if (pos + 1 >= len) return false;
            if (++hops > 32) return false; // guard against pointer loops
            size_t target = static_cast<size_t>((b & 0x3f) << 8) | data[pos + 1];
            if (!jumped) { after_pointer = pos + 2; jumped = true; }
            if (target >= len) return false;
            pos = target;
            continue;
        }
        if (b != 0 && (b & 0xc0) != 0) return false; // reserved label-length bits
        if (b == 0) { pos += 1; break; }
        size_t label_len = b;
        pos += 1;
        if (pos + label_len > len) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(data + pos), label_len);
        pos += label_len;
    }
    out += '.';
    end_offset = jumped ? after_pointer : pos;
    return true;
}

// DNS names compare case-insensitively (RFC 1035 3.1) -- real queriers
// don't reliably preserve whatever case this responder advertises in.
inline bool name_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

struct Question {
    std::string name;
    uint16_t qtype;
    bool unicast_response; // top bit of qclass -- the "QU" bit, RFC 6762 5.4
};

// Parses just the header's QDCOUNT and each Question -- this responder
// never needs to look at a query's Answer/Authority/Additional sections
// (queries don't carry meaningful ones for this responder's purposes),
// so those are deliberately not parsed at all.
inline bool parse_questions(const uint8_t* data, size_t len, std::vector<Question>& out) {
    if (len < 12) return false;
    uint16_t qdcount = static_cast<uint16_t>((data[4] << 8) | data[5]);
    size_t pos = 12;
    for (uint16_t i = 0; i < qdcount; ++i) {
        std::string name;
        size_t end_offset = 0;
        if (!parse_name(data, len, pos, name, end_offset)) return false;
        pos = end_offset;
        if (pos + 4 > len) return false;
        uint16_t qtype = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        uint16_t qclass = static_cast<uint16_t>((data[pos + 2] << 8) | data[pos + 3]);
        pos += 4;
        out.push_back(Question{name, qtype, (qclass & 0x8000) != 0});
    }
    return true;
}

// True if any parsed question is one this responder answers -- the
// exact qtype doesn't matter here (ANY, or the specific type, both
// count), since build_full_response below always answers with the
// complete record set regardless of which single name/type triggered
// it. Simpler and still entirely within spec: mDNS responders routinely
// send more than was strictly asked for.
inline bool matches_us(const std::vector<Question>& questions, const std::string& service_ptr_name,
                        const std::string& instance_name, const std::string& hostname) {
    for (const auto& q : questions) {
        if (name_eq(q.name, service_ptr_name) || name_eq(q.name, instance_name) || name_eq(q.name, hostname) ||
            name_eq(q.name, DNSSD_META_QUERY)) {
            return true;
        }
    }
    return false;
}

// True if every question that matched us specifically requested a
// unicast reply (QU bit set) -- if even one wants multicast (the
// default), the reply goes to the whole group instead, per RFC 6762
// 5.4's tie-breaking rule.
inline bool all_matches_want_unicast(const std::vector<Question>& questions, const std::string& service_ptr_name,
                                      const std::string& instance_name, const std::string& hostname) {
    bool any_match = false;
    for (const auto& q : questions) {
        bool is_match = name_eq(q.name, service_ptr_name) || name_eq(q.name, instance_name) ||
                        name_eq(q.name, hostname) || name_eq(q.name, DNSSD_META_QUERY);
        if (!is_match) continue;
        any_match = true;
        if (!q.unicast_response) return false;
    }
    return any_match;
}

// Builds one mDNS response packet advertising the full record set --
// used both for query-triggered responses and unsolicited announcements
// (RFC 6762 8.3). id=0 and an empty Question section match RFC 6762
// 6/18.1's guidance for multicast responses (this is also sent as-is
// for unicast replies -- real queriers accept either).
inline std::vector<uint8_t> build_full_response(const std::string& instance, uint16_t port,
                                                 const std::vector<std::string>& ipv4_addrs) {
    const std::string service_ptr_name = SERVICE_TYPE;
    const std::string instance_name = instance + "." + SERVICE_TYPE;
    const std::string hostname = instance + ".local.";

    std::vector<uint8_t> buf;
    write_u16(buf, 0);      // ID
    write_u16(buf, 0x8400); // flags: QR=1 (response), AA=1 (authoritative)
    write_u16(buf, 0);      // QDCOUNT

    uint16_t ancount = static_cast<uint16_t>(3 + ipv4_addrs.size()); // PTR, SRV, TXT, one A per address
    uint16_t arcount = 1;                                            // DNS-SD meta PTR, as an additional record
    write_u16(buf, ancount);
    write_u16(buf, 0); // NSCOUNT
    write_u16(buf, arcount);

    // PTR  _aipicam._tcp.local. -> <instance>._aipicam._tcp.local.
    write_name(buf, service_ptr_name);
    write_u16(buf, TYPE_PTR);
    write_u16(buf, CLASS_IN);
    write_u32(buf, 4500); // RFC 6762 10's recommended TTL for PTR records
    {
        std::vector<uint8_t> rdata;
        write_name(rdata, instance_name);
        write_u16(buf, static_cast<uint16_t>(rdata.size()));
        buf.insert(buf.end(), rdata.begin(), rdata.end());
    }

    // SRV  <instance>._aipicam._tcp.local. -> priority=0 weight=0 port target=<instance>.local.
    write_name(buf, instance_name);
    write_u16(buf, TYPE_SRV);
    write_u16(buf, CLASS_IN_FLUSH);
    write_u32(buf, 120); // RFC 6762 10's recommended TTL for records tied to network state
    {
        std::vector<uint8_t> rdata;
        write_u16(rdata, 0); // priority
        write_u16(rdata, 0); // weight
        write_u16(rdata, port);
        write_name(rdata, hostname);
        write_u16(buf, static_cast<uint16_t>(rdata.size()));
        buf.insert(buf.end(), rdata.begin(), rdata.end());
    }

    // TXT  <instance>._aipicam._tcp.local. -> (empty -- this API has no
    // key/value metadata worth advertising; a client just connects and
    // calls GET /status)
    write_name(buf, instance_name);
    write_u16(buf, TYPE_TXT);
    write_u16(buf, CLASS_IN_FLUSH);
    write_u32(buf, 120);
    write_u16(buf, 1);
    buf.push_back(0); // one zero-length string, RFC 6763 6.1's minimal valid TXT record

    // A  <instance>.local. -> each currently-known IPv4 address
    for (const auto& ip : ipv4_addrs) {
        write_name(buf, hostname);
        write_u16(buf, TYPE_A);
        write_u16(buf, CLASS_IN_FLUSH);
        write_u32(buf, 120);
        write_u16(buf, 4);
        in_addr addr{};
        inet_pton(AF_INET, ip.c_str(), &addr);
        uint32_t host_order = ntohl(addr.s_addr);
        write_u32(buf, host_order);
    }

    // PTR  _services._dns-sd._udp.local. -> _aipicam._tcp.local. (additional record --
    // lets generic "what's on this network" browsing tools find the service type at all)
    write_name(buf, DNSSD_META_QUERY);
    write_u16(buf, TYPE_PTR);
    write_u16(buf, CLASS_IN);
    write_u32(buf, 4500);
    {
        std::vector<uint8_t> rdata;
        write_name(rdata, service_ptr_name);
        write_u16(buf, static_cast<uint16_t>(rdata.size()));
        buf.insert(buf.end(), rdata.begin(), rdata.end());
    }

    return buf;
}

// ---------------------------------------------------------------------
// Socket layer -- Linux-only (IP_PKTINFO/ip_mreqn), see this file's
// header comment. Guarded so the pure wire-format functions above
// (write_name/parse_name/parse_questions/build_full_response) can still
// be included and exercised directly on a non-Linux machine -- exactly
// how this file's own DNS message encoding was verified against
// Apple's real `dns-sd` tool on macOS before being wired into this
// class; see this project's PR history for that standalone harness.
#ifdef __linux__

class MdnsResponder {
public:
    MdnsResponder(std::string instance, uint16_t port) : instance_(std::move(instance)), port_(port) {}

    bool start(std::string& err) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            err = std::string("socket() failed: ") + strerror(errno);
            return false;
        }

        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(MDNS_PORT);
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            err = std::string("bind(:5353) failed: ") + strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }

        // RFC 6762 5: mDNS packets must always use TTL 255 and must
        // never be forwarded by a router -- this isn't a routing hop
        // budget, just a spec-mandated fixed value that lets a receiver
        // reject anything a router forwarded anyway as non-conformant.
        unsigned char ttl = 255;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        // This process should never see its own transmitted multicast
        // packets come back in as a "query" -- disabling loopback avoids
        // a self-triggered response loop entirely, rather than needing
        // to detect and ignore our own packets after the fact.
        unsigned char loop = 0;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        refresh_interfaces();

        running_ = true;
        recv_thread_ = std::thread([this]() { recv_loop(); });
        announce_thread_ = std::thread([this]() { announce_loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (fd_ >= 0) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
        if (recv_thread_.joinable()) recv_thread_.join();
        if (announce_thread_.joinable()) announce_thread_.join();
    }

private:
    // Joins the mDNS multicast group on every active, non-loopback IPv4
    // interface, and snapshots their current addresses for building A
    // records. Safe to call repeatedly (e.g. from the periodic refresh
    // below) -- re-joining an interface already joined just fails with
    // EADDRINUSE, which is ignored, not a real error; this is how a
    // *newly*-appeared interface (WiFi associating after this responder
    // already started, Ethernet being plugged in later) gets picked up
    // without restarting anything.
    bool refresh_interfaces() {
        std::vector<std::string> addrs;
        ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) != 0) return false;
        for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST)) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;

            unsigned ifindex = if_nametoindex(ifa->ifa_name);
            if (ifindex == 0) continue;

            ip_mreqn mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
            mreq.imr_address.s_addr = INADDR_ANY;
            mreq.imr_ifindex = static_cast<int>(ifindex);
            setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

            auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
                addrs.emplace_back(ip);
            }
        }
        freeifaddrs(ifap);

        std::lock_guard<std::mutex> lk(addrs_mu_);
        bool changed = addrs != addrs_;
        addrs_ = addrs;
        return changed;
    }

    std::vector<std::string> current_addrs() {
        std::lock_guard<std::mutex> lk(addrs_mu_);
        return addrs_;
    }

    void announce_loop() {
        // RFC 6762 8.3: announce at least twice, one second apart, on
        // startup, so a client already browsing notices immediately
        // rather than waiting for its next periodic query.
        send_multicast(build_full_response(instance_, port_, current_addrs()));
        for (int i = 0; i < 2 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running_) break;
            send_multicast(build_full_response(instance_, port_, current_addrs()));
        }

        // Thereafter, only re-announce when this Pi's own address set
        // actually changes (WiFi joining/leaving, AP mode toggling,
        // Ethernet appearing) -- not on every tick, which would just
        // needlessly spam the network with identical, unchanged data.
        while (running_) {
            for (int i = 0; i < 100 && running_; ++i) { // ~10s, in short slices so stop() isn't delayed
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) break;
            if (refresh_interfaces()) {
                send_multicast(build_full_response(instance_, port_, current_addrs()));
            }
        }
    }

    void recv_loop() {
        std::vector<uint8_t> buf(4096);
        while (running_) {
            iovec iov{};
            iov.iov_base = buf.data();
            iov.iov_len = buf.size();

            char cmsg_buf[CMSG_SPACE(sizeof(in_pktinfo))];
            sockaddr_in from{};

            msghdr msg{};
            msg.msg_name = &from;
            msg.msg_namelen = sizeof(from);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            ssize_t n = recvmsg(fd_, &msg, 0);
            if (n <= 0) {
                if (!running_) break;
                continue;
            }

            std::vector<Question> questions;
            if (!parse_questions(buf.data(), static_cast<size_t>(n), questions)) continue;

            const std::string service_ptr_name = SERVICE_TYPE;
            const std::string instance_name = instance_ + "." + SERVICE_TYPE;
            const std::string hostname = instance_ + ".local.";
            if (!matches_us(questions, service_ptr_name, instance_name, hostname)) continue;

            in_pktinfo* pi = nullptr;
            for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                    pi = reinterpret_cast<in_pktinfo*>(CMSG_DATA(cmsg));
                    break;
                }
            }

            auto response = build_full_response(instance_, port_, current_addrs());
            bool unicast = all_matches_want_unicast(questions, service_ptr_name, instance_name, hostname);
            if (unicast) {
                send_to(response, from, pi);
            } else {
                send_multicast(response, pi);
            }
        }
    }

    // Sends out the same interface a query arrived on (via IP_PKTINFO),
    // so a reply on a multi-homed Pi (wlan0 + eth0 both up) goes back
    // out the interface it's actually relevant to, not whichever
    // interface the kernel's default route happens to prefer.
    void send_with_pktinfo(const std::vector<uint8_t>& packet, const sockaddr_in& dest, const in_pktinfo* pi) {
        iovec iov{};
        iov.iov_base = const_cast<uint8_t*>(packet.data());
        iov.iov_len = packet.size();

        msghdr msg{};
        msg.msg_name = const_cast<sockaddr_in*>(&dest);
        msg.msg_namelen = sizeof(dest);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        char cmsg_buf[CMSG_SPACE(sizeof(in_pktinfo))];
        if (pi) {
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);
            cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            auto* out_pi = reinterpret_cast<in_pktinfo*>(CMSG_DATA(cmsg));
            *out_pi = *pi;
        }

        sendmsg(fd_, &msg, 0);
    }

    void send_multicast(const std::vector<uint8_t>& packet, const in_pktinfo* pi = nullptr) {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(MDNS_PORT);
        dest.sin_addr.s_addr = inet_addr(MDNS_ADDR);
        if (pi) {
            send_with_pktinfo(packet, dest, pi);
        } else {
            // No specific interface known (e.g. a periodic re-announce,
            // not triggered by an incoming query) -- send on every
            // active interface individually rather than relying on
            // whichever one the kernel's default multicast route picks,
            // so this Pi is discoverable from *any* of its networks
            // (wlan0's AP or station address, and eth0), not just one.
            for (const auto& addr : current_addrs()) {
                in_pktinfo synth{};
                in_addr local{};
                inet_pton(AF_INET, addr.c_str(), &local);
                synth.ipi_spec_dst = local;
                send_with_pktinfo(packet, dest, &synth);
            }
        }
    }

    void send_to(const std::vector<uint8_t>& packet, const sockaddr_in& dest, const in_pktinfo* pi) {
        send_with_pktinfo(packet, dest, pi);
    }

    std::string instance_;
    uint16_t port_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread announce_thread_;
    std::mutex addrs_mu_;
    std::vector<std::string> addrs_;
};

#endif // __linux__

} // namespace mdns
