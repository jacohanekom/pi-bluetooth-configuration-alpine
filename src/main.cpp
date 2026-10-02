/**
 * pi-bluetooth-configuration
 * ===========================
 * Lets you join this Pi to a WiFi network from a plain browser -- no
 * phone app, no SSH, no keyboard, no second board, never BLE, and (as
 * of this design) never a WiFi access point either. wlan0 stays in
 * station mode at all times: on startup this daemon tries to join
 * whatever's already configured (wpa_supplicant, started by OpenRC
 * before this daemon, attempts that entirely on its own). The web UI
 * (GET /, see web_ui.hpp) is served over whatever network connectivity
 * the device already has -- eth0's own addressing (static or DHCP) is
 * entirely preconfigured by the SD-card image itself (see
 * sdcard-image-pi3/sdcard-image-pi-zero), not managed by this daemon at
 * all -- so there is nothing here that brings up a gateway IP or a DHCP
 * server. Found automatically via mDNS/Bonjour (see mdns_responder.hpp)
 * rather than requiring its address to be typed in.
 *
 * This replaces three earlier designs entirely (see git history): a
 * direct BlueZ/D-Bus GATT peripheral; offloading BLE to a Raspberry Pi
 * Pico 2 W over USB serial after BlueZ's own built-in GATT profiles
 * proved to force a disconnect loop no userspace config could fix; and
 * a hostapd-driven fallback access point a phone joined directly, which
 * worked but made the WiFi-join flow strictly harder than it needed to
 * be (this radio can't run AP and station mode at once, so submitting
 * credentials while that AP was active could only ever stage them for a
 * later join, never attempt one live, without severing the very
 * connection the request arrived over). This daemon used to also own
 * eth0's static IP/DHCP server/NAT directly (bridging it with a second
 * wired interface if configured) -- that was removed too: it's now the
 * SD-card image's job to preconfigure networking once at build time,
 * not this daemon's job to reconfigure it live.
 *
 * WiFi, relay control, and login accounts are all entirely independent
 * features, not sequenced steps in a wizard -- there is no "finished
 * setup" state anymore, no marker file gating any of them, and no
 * reboot anywhere in this daemon. Each can be configured any time, in
 * any order, regardless of the others' state.
 *
 * Login accounts are plain, user-chosen Unix accounts (see "Logging in"
 * below) -- there is no auto-generated admin account anymore. Nothing
 * creates a login for you; you add one yourself, with a username and
 * password you choose, from the web UI's "Users" section.
 *
 * HTTP API (JSON; see the route table in main() for the exact shapes):
 *   GET  /         the browser-based web UI itself (see web_ui.hpp) --
 *                   a single static page that talks to the routes below.
 *   GET  /status    combined snapshot -- wifi state, relay states,
 *                   Victron telemetry, and the last scan's results. No
 *                   server push: clients are expected to poll this
 *                   periodically instead.
 *   POST /scan      triggers a background WiFi scan; poll GET /status
 *                   for results once it finishes (a few seconds later).
 *   POST /connect   {"ssid":...,"password":...} -- joins the given
 *                   network directly and synchronously in the
 *                   background; poll GET /status's wifi.state for the
 *                   outcome.
 *   POST /forget    forgets the configured network. wlan0 stays in
 *                   station mode, simply idle, until POST /connect is
 *                   called again.
 *   GET  /accounts  lists the Unix accounts this daemon has created
 *                   (see add_account/list_accounts).
 *   POST /accounts  {"username":...,"password":...} -- creates a new
 *                   Unix account with doas access to root; see
 *                   add_account().
 *   POST /accounts/remove  {"username":...} -- deletes an account this
 *                   daemon created; see remove_account().
 *   POST /change-password  {"username":...,"currentPassword":...,
 *                   "newPassword":...} -- change your own password;
 *                   see change_password(). The only way to clear
 *                   root's forced-change gate -- see
 *                   ROOT_PASSWORD_CHANGED_FILE's own comment.
 *   POST /ssh       {"enabled":bool} -- starts/stops sshd and adds/
 *                   removes it from the default runlevel, live.
 *   POST /relay     {"port":...,"state":"on"|"off"} -- see relay_control.hpp.
 *   POST /relay-control  {"enabled":bool} -- master on/off switch for
 *                   the whole relay integration, persisted to config.ini.
 *   POST /relay-always-on  {"port":...,"alwaysOn":bool} -- edits
 *                   pi-relay-control-alpine's own config file and
 *                   restarts it; see relay_control.hpp.
 *   POST /user      {"name":...,"email":...} -- purely informational,
 *                   labels the device; stored in CAMERA_USER_FILE. Not
 *                   used for access control anywhere -- unrelated to the
 *                   Unix accounts /accounts manages, despite the
 *                   similar-looking name.
 *
 * Every route requires HTTP Basic Auth (see auth.hpp), checked against
 * this device's own real /etc/shadow -- the same root account and
 * whatever POST /accounts has created, no separate credential store.
 * Still plain, unencrypted HTTP otherwise: this project's security
 * model (see the README) already treats the WiFi-configuration flow as
 * suitable for a trusted home/lab environment only, not a public one;
 * this means credentials (both the HTTP Basic Auth kind and WiFi's own)
 * cross this API in the clear. Reachable only from whatever network
 * connectivity the device already has (eth0, preconfigured by the
 * SD-card image, or an already-joined WiFi network), not broadcast over
 * the air the way the old fallback-AP design was.
 *
 * Relay control is a separate, optional integration with
 * pi-relay-control-alpine: this daemon doesn't drive GPIO itself, it
 * just forwards on/off to whichever relay is listening on that TCP port
 * on 127.0.0.1, and reports live state back via GET /status. Always
 * available (gated only by the relays_enabled runtime/config toggle --
 * see POST /relay-control -- never by WiFi/setup state). See
 * relay_control.hpp and the README's "Relay control" section for the
 * "[relays]" config format that maps ports to display labels.
 *
 * Victron solar/battery telemetry is a similar optional integration,
 * this time with victron-ve-direct-alpine: queries its status control
 * port for the latest reading and republishes it as JSON. Read-only,
 * always live. See victron_control.hpp.
 *
 * Build (Alpine Linux):
 *   make
 * Run:
 *   ./pi-bluetooth-configuration [--config config.ini]
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/time.h>

#include "auth.hpp"
#include "config.hpp"
#include "http_server.hpp"
#include "mdns_responder.hpp"
#include "relay_control.hpp"
#include "victron_control.hpp"
#include "web_ui.hpp"
#include "wifi_control.hpp"

namespace {

constexpr const char* HOSTNAME_FILE = "/etc/hostname";
// Purely informational -- whoever the device is labeled as gets POSTed
// here once and stored. Under /etc, not bare at the filesystem root --
// diskless installs (see sdcard-image-pi3) persist config via `apk
// audit --backup` (what `lbu commit` actually uses under the hood),
// which reliably tracks new/changed files *within* a protected
// directory like /etc but -- confirmed directly, not assumed --
// silently never picks up a bare top-level file no matter how it's
// listed in protected_paths.d/*.list. Not used for access control --
// nothing gates on this file existing.
constexpr const char* CAMERA_USER_FILE = "/etc/camera_user";

// Every account this daemon has created gets its own file here, named
// "<PREFIX><username><SUFFIX>" -- e.g.
// "/etc/doas.d/pi-bluetooth-configuration-user-alice.conf" -- rather
// than a shared /etc/doas.conf edit or a single well-known admin-user
// file (the old single-auto-generated-account design this replaced).
// This directory listing itself (see list_accounts()) is the one and
// only source of truth for "which accounts does this daemon manage" --
// no separate index file to keep in sync, and no risk of ever touching
// an account this daemon didn't create itself (root, nobody, any
// package-created system account): remove_account() refuses anything
// without a matching file here. Confirmed directly against a real doas
// that /etc/doas.d/*.conf is genuinely additive to /etc/doas.conf
// (Alpine-specific; not a bare upstream-OpenBSD-doas feature).
constexpr const char* DOAS_USER_CONF_DIR    = "/etc/doas.d";
constexpr const char* DOAS_USER_CONF_PREFIX = "pi-bluetooth-configuration-user-";
constexpr const char* DOAS_USER_CONF_SUFFIX = ".conf";

// The symlink OpenRC's `rc-update add sshd default` creates -- its mere
// existence is exactly what `rc-update show default` itself checks, so
// reading it directly here (rather than shelling out to parse that
// command's output) is both simpler and exactly as authoritative.
constexpr const char* SSHD_RUNLEVEL_LINK = "/etc/runlevels/default/sshd";

// Marks that `root`'s password has been changed at least once via
// POST /change-password since this device was imaged -- root starts out
// with `ROOT_PASSWORD` (set once, at build time, often reused verbatim
// across every device built from the same image), so unlike any account
// POST /accounts creates (always a password someone chose fresh), root
// genuinely has a "default" credential worth forcing a change away from
// the first time it's used to log into the web UI. An empty marker file
// under /etc/pi-bluetooth-configuration (that directory already exists
// -- config.ini is installed there) rather than comparing shadow hashes
// against a build-time snapshot: simpler, and consistent with this
// project's existing "file exists = state is true" convention
// elsewhere (DOAS_USER_CONF_DIR's own files).
constexpr const char* ROOT_PASSWORD_CHANGED_FILE = "/etc/pi-bluetooth-configuration/root-password-changed";

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

// Same bare-bones extraction as json_get_string/json_get_int above --
// looks for an unquoted `true`/`false` token right after the key's
// colon, matching the shape of this daemon's own JSON responses (see
// e.g. POST /relay-control's own body).
bool json_get_bool(const std::string& body, const std::string& key, bool def) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return def;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (body.compare(pos, 4, "true") == 0) return true;
    if (body.compare(pos, 5, "false") == 0) return false;
    return def;
}

std::string status_json(const WifiStatus& s) {
    std::ostringstream o;
    o << "{\"state\":\"" << s.state_name() << "\","
      << "\"ssid\":\"" << escape_json(s.ssid) << "\","
      << "\"ip\":\"" << escape_json(s.ip) << "\","
      << "\"error\":\"" << escape_json(s.error) << "\"}";
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

// Queries each configured relay's live state (via relay_control.hpp,
// one TCP round-trip per relay to pi-relay-control-alpine) every time
// this is called -- simple, and there are only ever a handful of
// relays, so this is cheap enough to run on every GET /status. Unlike
// the old BLE-era design, there's no shared dispatch thread this could
// block (http_server.hpp is thread-per-connection -- a slow relay query
// only ever delays its own request), so the caching layer that used to
// exist here for exactly that reason is gone.
//
// `enabled` reflects the runtime on/off switch (see main()'s own
// relays_enabled) -- when false, every relay is reported "disabled"
// without a live query at all, both to avoid pointless TCP round-trips
// to a feature the user just turned off and so the web UI can grey out
// each relay row distinctly from a real "unknown" (not reachable). The
// relay *list itself* (ports/labels) is still reported either way, so a
// disabled toggle can be turned back on from the web UI without losing
// sight of what it controls.
//
// `alwaysOn` is read fresh from pi-relay-control-alpine's own config
// file on every call (relayctl::relay_always_on) -- cheap (a small local
// file, not a network round-trip) and always correct even if something
// other than this daemon edited it by hand.
std::string relays_json(const std::vector<relayctl::RelayConfig>& relays,
                         std::map<int, std::mutex>& port_mu, bool enabled) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < relays.size(); ++i) {
        if (i) o << ",";
        std::string state;
        if (!enabled) {
            state = "disabled";
        } else {
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
          << "\"state\":\"" << state << "\","
          << "\"alwaysOn\":" << (relayctl::relay_always_on(relays[i].port) ? "true" : "false") << "}";
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

// Plain "key=value" lines, one per field -- same convention as this
// project's own config.ini/pi-relay-control.conf rather than JSON,
// since this is meant to be just as easy to read/edit by hand on the
// device as those. Embedded newlines are stripped from each value
// (not otherwise expected in a name/email, but a client sending one
// shouldn't be able to inject a fake extra line into the file).
void write_camera_user(const std::string& name, const std::string& email) {
    auto sanitize = [](std::string v) {
        v.erase(std::remove(v.begin(), v.end(), '\n'), v.end());
        v.erase(std::remove(v.begin(), v.end(), '\r'), v.end());
        return v;
    };
    std::ofstream f(CAMERA_USER_FILE);
    f << "name=" << sanitize(name) << "\n";
    f << "email=" << sanitize(email) << "\n";
}

std::string doas_user_conf_path(const std::string& user) {
    return std::string(DOAS_USER_CONF_DIR) + "/" + DOAS_USER_CONF_PREFIX + user + DOAS_USER_CONF_SUFFIX;
}

// Conservative Unix username rules (lowercase letters/digits/"_"/"-",
// starting with a letter or "_", capped well under every adduser
// implementation's own limit) -- checked here mainly so a bad username
// fails fast with a clear error instead of some more confusing failure
// from adduser itself, and so it's always safe to embed directly into
// doas_user_conf_path()'s filename with nothing to escape.
bool valid_username(const std::string& u) {
    if (u.empty() || u.size() > 32) return false;
    if (!(std::islower(static_cast<unsigned char>(u[0])) || u[0] == '_')) return false;
    for (char c : u) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::islower(uc) || std::isdigit(uc) || c == '_' || c == '-')) return false;
    }
    return true;
}

// The one and only source of truth for "which accounts does this daemon
// manage" -- see DOAS_USER_CONF_PREFIX's own comment for why this reads
// the directory itself rather than a separate index file. Sorted so the
// web UI's own list doesn't reorder itself between polls for no reason.
std::vector<std::string> list_accounts() {
    std::vector<std::string> accounts;
    DIR* dir = opendir(DOAS_USER_CONF_DIR);
    if (!dir) return accounts;
    std::string prefix = DOAS_USER_CONF_PREFIX;
    std::string suffix = DOAS_USER_CONF_SUFFIX;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        accounts.push_back(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
    }
    closedir(dir);
    std::sort(accounts.begin(), accounts.end());
    return accounts;
}

// Creates a plain Unix account with the given username/password (both
// user-chosen -- unlike the single auto-generated admin account this
// replaced, there's no credential-reveal step here at all: the caller
// already knows the password, since they just typed it in) and permits
// it to `doas` (Alpine's sudo-equivalent) to root. Refuses up front if
// an account by this name already exists as far as this daemon's own
// tracking is concerned (see list_accounts()'s own comment) -- a
// same-named *system* account (root, nobody, ...) that this daemon
// never created is instead caught naturally by `adduser` itself
// refusing outright, surfaced below as a normal failure.
bool add_account(const std::string& user, const std::string& pass, std::string& err) {
    if (!valid_username(user)) {
        err = "invalid username -- lowercase letters/digits/\"_\"/\"-\" only, starting with a letter or \"_\"";
        return false;
    }
    std::string conf_path = doas_user_conf_path(user);
    if (std::ifstream(conf_path).good()) {
        err = "an account named \"" + user + "\" already exists";
        return false;
    }

    auto add = run_command({"adduser", "-D", "-h", "/home/" + user, user});
    if (add.exit_code != 0) {
        err = "failed to create user: " + trim(add.output);
        return false;
    }
    // Same SHA-512 crypt mechanism build-image.sh's own ROOT_PASSWORD
    // uses at build time (`openssl passwd -6`) rather than
    // reimplementing crypt(3) here. Passed as a plain argv element, not
    // stdin, for the same reason build-image.sh does: execvp has no
    // shell to leak it through, and this device has no untrusted local
    // users who could read another root process's /proc/<pid>/cmdline
    // during the sub-second window this runs.
    auto hashed = run_command({"openssl", "passwd", "-6", pass});
    if (hashed.exit_code != 0) {
        err = "failed to hash password: " + trim(hashed.output);
        run_command({"deluser", user});
        return false;
    }
    // usermod (from the `shadow` package -- busybox's own adduser/
    // passwd/chpasswd don't include it) is the one tool that accepts an
    // already-computed hash directly via argv; chpasswd -e can also set
    // a pre-hashed password but only via stdin, which run_command()
    // doesn't wire up (see subprocess.hpp) -- this avoids needing to.
    auto set_pass = run_command({"usermod", "-p", trim(hashed.output), user});
    if (set_pass.exit_code != 0) {
        err = "failed to set password: " + trim(set_pass.output);
        run_command({"deluser", user});
        return false;
    }

    // "permit persist" mirrors what most sudo setups do (re-prompt
    // periodically, not on literally every invocation) rather than
    // plain "permit" (every time) or "permit nopass" (never -- which
    // would make a stolen SSH session immediately root-equivalent with
    // no further credential check).
    {
        std::ofstream f(conf_path);
        f << "permit persist " << user << "\n";
    }
    // doas refuses to honor a config file it doesn't own outright or
    // that's group/other-writable -- confirmed directly against a real
    // doas, not assumed from its docs.
    run_command({"chown", "root:root", conf_path});
    run_command({"chmod", "0600", conf_path});
    return true;
}

// Refuses anything without a matching doas.d file -- see
// DOAS_USER_CONF_PREFIX's own comment on why that's the safe way to
// guarantee this can never be pointed at an account this daemon didn't
// create itself.
bool remove_account(const std::string& user, std::string& err) {
    std::string conf_path = doas_user_conf_path(user);
    if (!std::ifstream(conf_path).good()) {
        err = "no account named \"" + user + "\" is managed by this daemon";
        return false;
    }
    run_command({"deluser", user});
    std::remove(conf_path.c_str());
    return true;
}

std::string accounts_json(const std::vector<std::string>& accounts) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < accounts.size(); ++i) {
        if (i) o << ",";
        o << "\"" << escape_json(accounts[i]) << "\"";
    }
    o << "]";
    return o.str();
}

// Reflects whether `rc-update add sshd default` has been run -- see
// SSHD_RUNLEVEL_LINK's own comment.
bool sshd_enabled() {
    return std::ifstream(SSHD_RUNLEVEL_LINK).good();
}

// See ROOT_PASSWORD_CHANGED_FILE's own comment.
bool root_password_changed() {
    return std::ifstream(ROOT_PASSWORD_CHANGED_FILE).good();
}

// Changes any account's own password -- the caller must already know
// the *current* one (verified fresh here via authctl::verify_password,
// independent of whatever Basic Auth credentials the HTTP request
// itself carried) -- this is "change your own password", not an admin
// reset of someone else's; main.cpp's own POST /change-password route
// enforces that distinction by refusing to even call this unless the
// Basic-Auth-authenticated user matches the account being changed. Sets
// ROOT_PASSWORD_CHANGED_FILE the first time root's own password is
// changed this way, satisfying the force-a-change-on-first-login gate
// (see that file's own comment) -- never touched for any other account,
// which has no such gate to satisfy.
bool change_password(const std::string& user, const std::string& current_password,
                      const std::string& new_password, std::string& err) {
    if (!authctl::verify_password(user, current_password)) {
        err = "current password is incorrect";
        return false;
    }
    if (new_password.empty()) {
        err = "new password is required";
        return false;
    }
    if (new_password == current_password) {
        err = "new password must be different from the current one";
        return false;
    }

    // Same hash-then-usermod sequence as add_account() -- see that
    // function's own comments for why each step is shaped this way
    // (openssl over crypt(3), argv over stdin, etc.).
    auto hashed = run_command({"openssl", "passwd", "-6", new_password});
    if (hashed.exit_code != 0) {
        err = "failed to hash password: " + trim(hashed.output);
        return false;
    }
    auto set_pass = run_command({"usermod", "-p", trim(hashed.output), user});
    if (set_pass.exit_code != 0) {
        err = "failed to set password: " + trim(set_pass.output);
        return false;
    }

    if (user == "root") {
        std::ofstream(ROOT_PASSWORD_CHANGED_FILE).close();
    }
    return true;
}

// Returns null if CAMERA_USER_FILE doesn't exist yet (no one has ever
// POSTed /user) rather than an object with empty fields, so the client
// can tell "not set" apart from "set to blank".
std::string camera_user_json() {
    std::ifstream f(CAMERA_USER_FILE);
    if (!f.is_open()) return "null";
    std::string name, email, line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "name") name = val;
        else if (key == "email") email = val;
    }
    std::ostringstream o;
    o << "{\"name\":\"" << escape_json(name) << "\","
      << "\"email\":\"" << escape_json(email) << "\"}";
    return o.str();
}

// The board's hardware serial (from /proc/cpuinfo) rather than a fixed
// configured name, so multiple aipicam units are distinguishable from
// each other (system hostname, mDNS service name) instead of all
// showing the same name. Falls back to wifi.device_name (e.g. when not
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

// Sets the running hostname to the same hardware serial already used
// for the AP SSID and device_id, so `ssh root@<serial>.local` matches
// what a phone sees in its WiFi list and in mDNS -- rather than every
// unit sharing one generic baked-in hostname. Idempotent (the serial
// never changes), so this just runs unconditionally on every startup
// instead of needing a "first boot only" marker. Also rewrites
// /etc/hostname directly (`hostname` alone only changes the live
// kernel value) so a diskless install's next `lbu commit` persists it
// -- harmless no-op on a disk-resident install too.
void set_hostname_from_serial(const std::string& serial) {
    if (serial.empty()) return;
    auto r = run_command({"hostname", serial});
    if (r.exit_code != 0) {
        std::cerr << "[Main] failed to set hostname to \"" << serial << "\": " << trim(r.output) << "\n";
        return;
    }
    std::ofstream(HOSTNAME_FILE) << serial << "\n";
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
    set_hostname_from_serial(serial);
    const std::string iface      = cfg.get_str("wifi.interface", "wlan0");
    const int sta_boot_timeout_secs = cfg.get_int("wifi.connect_timeout_secs", 20);
    const int max_scan_results   = cfg.get_int("scan.max_results", 10);
    const auto relays = relayctl::load_relays(cfg_path);

    // Runtime on/off switch for the whole relay integration, seeded from
    // config.ini at startup but flippable live from the web UI (see
    // POST /relay-control below) without needing an edit + restart --
    // set_relays_enabled() keeps the on-disk value in sync with this at
    // the same time, so a reboot doesn't silently revert a web UI
    // change. Independent of `relays` itself, which stays fixed for the
    // life of the process -- see load_relays()'s own comment.
    std::atomic<bool> relays_enabled{relayctl::relays_enabled(cfg_path)};

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
    const int http_port      = cfg.get_int("http.port", 8080);

    std::cerr << "[Config] device   : " << dev_name << (serial.empty() ? " (configured)" : " (hardware serial)") << "\n"
              << "[Config] wifi if  : " << iface << "\n"
              << "[Config] http port: " << http_port << "\n"
              << "[Config] relays   : " << relays.size() << " configured\n"
              << "[Config] victron  : ctrl_port " << victron_ctrl_port << "\n";

    WifiControl wifi(iface);

    // Advertised as soon as possible, independent of WiFi's own boot
    // sequence below -- it adapts to whichever interfaces/addresses
    // actually come and go on its own (see
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

    std::mutex scan_mu;
    std::string last_scan_json = "[]";

    // Tries to join whatever's already configured -- wpa_supplicant,
    // already started by OpenRC before this daemon (see its own
    // depend()), attempts this entirely on its own; this just waits to
    // see whether it succeeds within a bounded time. Covers both "wrong
    // password/network out of range" and "nothing configured at all yet"
    // the same way: either one just fails to reach CONNECTED before the
    // timeout. Either way wlan0 stays in station mode -- there is no
    // fallback AP to fall through to anymore (see this file's own header
    // comment for why): the web UI, reachable over whatever network
    // connectivity the device already has regardless of WiFi's own
    // state, is what a fresh/unconfigured device is configured through
    // instead.
    bool sta_ok = false;
    for (int i = 0; i < sta_boot_timeout_secs * 2; ++i) {
        if (wifi.get_status().state == WifiStatus::CONNECTED) { sta_ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (sta_ok) {
        std::cerr << "[Wifi] joined " << wifi.get_status().ssid << " on boot\n";
    } else {
        std::cerr << "[Wifi] no configured network joined within " << sta_boot_timeout_secs
                   << "s -- use the web UI to configure one\n";
    }

    // Seeds the scan list shown on the web UI's very first load, before
    // anyone has clicked "Scan" yet -- harmless either way since the
    // radio is always in station mode now (no AP mode to conflict with,
    // unlike the earlier fallback-AP design this replaced).
    std::thread([&]() {
        InflightGuard guard;
        auto results = wifi.scan(max_scan_results);
        std::lock_guard<std::mutex> lk(scan_mu);
        last_scan_json = scan_json(results);
    }).detach();

    httpsrv::HttpServer server;

    auto do_scan = [&]() {
        auto results = wifi.scan(max_scan_results);
        std::lock_guard<std::mutex> lk(scan_mu);
        last_scan_json = scan_json(results);
    };

    // wlan0 stays in station mode throughout, so nothing about however
    // this request reached the daemon (never the radio being
    // reconfigured) is disrupted by this. lbu-commits its own result
    // (via wifi_control.hpp's backup_wpa_conf, called from connect() on
    // success) -- WiFi is independent of relay state now, so there's no
    // later "finish" step to batch that up for it anymore.
    auto do_connect = [&](const std::string& ssid, const std::string& psk) {
        wifi.connect(ssid, psk);
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after connect: " << trim(commit.output) << "\n";
    };

    // No reboot -- an earlier version of this rebooted into a fresh
    // diskless boot to get back to a clean state, but that meant every
    // reset/reconfigure paid diskless mode's own full package-reinstall
    // cost, and (worse) went through exactly the boot-time
    // wpa_supplicant.conf-gets-clobbered-then-restored dance that's
    // already this image's single flakiest sequence (see "Config
    // persistence across reboots"). Doing it live instead -- the same
    // wifi.forget() call this daemon already has, just invoked directly
    // rather than deferred to the next boot -- sidesteps that whole class
    // of risk, not just this specific bug. wlan0 simply goes idle
    // afterward (station mode, no network selected) until POST /connect
    // is called again -- there's no AP mode to fall back into anymore.
    // Purely a WiFi action now -- doesn't touch relay control or login
    // accounts, both independent features of their own.
    auto do_forget = [&]() {
        wifi.forget();
        // On Alpine diskless installs (see sdcard-image-pi3), root is
        // tmpfs -- none of what just changed under /etc would survive a
        // future reboot without this. -d is load-bearing, not cosmetic:
        // `lbu commit`'s own target filename is
        // "$(hostname).apkovl.tar.gz" (confirmed by reading alpine-conf's
        // lbu.in directly), computed from the CURRENT hostname at commit
        // time -- but set_hostname_from_serial() already renamed the
        // live hostname away from the image's build-time one (e.g.
        // "aipicam") long before this ever runs. Without -d, lbu commit
        // finds that mismatch (the existing on-disk apkovl was named
        // after the OLD hostname) and refuses outright ("more than one
        // apkovl file(s) were found ... Please use -d to replace"),
        // rather than silently doing nothing -- meaning every commit was
        // failing outright, confirmed against a real device. -d tells it
        // to just replace whatever apkovl(s) already exist with the
        // current one, which is exactly what a single-owner device wants
        // regardless of what its hostname was at build time vs. now.
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after forget: " << trim(commit.output) << "\n";
    };

    // Starts/stops sshd live and adds/removes it from the default
    // runlevel in the same call, so "enabled" means the same thing
    // immediately and after the next reboot -- no separate "apply now"
    // vs. "apply on next boot" distinction to expose to the web UI.
    auto do_set_ssh_enabled = [&](bool enabled) -> bool {
        if (enabled) {
            run_command({"rc-update", "add", "sshd", "default"});
            auto r = run_command({"rc-service", "sshd", "start"}, 15);
            if (r.exit_code != 0) {
                std::cerr << "[SSH] failed to start sshd: " << trim(r.output) << "\n";
                return false;
            }
        } else {
            auto r = run_command({"rc-service", "sshd", "stop"}, 15);
            if (r.exit_code != 0) {
                std::cerr << "[SSH] failed to stop sshd: " << trim(r.output) << "\n";
            }
            run_command({"rc-update", "del", "sshd", "default"});
        }
        return true;
    };

    // None of these boards have a battery-backed RTC (see
    // sdcard-image-pi3/README.md's "System clock reliability") -- every
    // cold boot starts with whatever time the kernel happens to have,
    // often long in the past, until chronyd corrects it over NTP. That
    // needs real internet access, which a freshly unconfigured device
    // with no WiFi joined yet doesn't have -- so a wrong clock can
    // otherwise persist for the entire time setup is being run, which
    // matters because it also breaks HTTPS certificate-date validation
    // for anything this daemon itself does over HTTPS. The client's own
    // clock is essentially always correct by comparison (carrier/OS-
    // synced), so it can just hand it over directly instead of waiting
    // on NTP. No reboot needed -- `date -u -s` takes effect immediately,
    // same live-in-place philosophy as everything else this daemon does
    // post-boot.
    auto do_set_time = [&](long unix_time) -> bool {
        // A direct settimeofday() call, not a `date -u -s` subprocess --
        // confirmed directly against Alpine's own BusyBox date applet
        // that it prints "can't set date: Operation not permitted" to
        // stderr yet still exits 0 when the underlying syscall itself is
        // refused (e.g. no CAP_SYS_TIME), which would have made
        // run_command()'s usual exit_code check silently report success
        // for a clock that was never actually changed. settimeofday()
        // itself reports that failure reliably via errno -- trust the
        // syscall's own error reporting over a subprocess's exit code
        // whenever a shelled-out tool's own success/failure signaling
        // can't be trusted.
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(unix_time);
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) != 0) {
            std::cerr << "[Time] failed to set system clock: " << std::strerror(errno) << "\n";
            return false;
        }
        // Best-effort and expected to fail on every board this image
        // targets (no RTC hardware to write to at all) -- not worth
        // failing the whole request over; chronyd will happily correct
        // this again over NTP once real internet access exists anyway.
        run_command({"hwclock", "-w"});
        std::time_t t = static_cast<std::time_t>(unix_time);
        std::tm tm_utc{};
        gmtime_r(&t, &tm_utc);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
        std::cerr << "[Time] system clock set to " << buf << " UTC\n";
        return true;
    };

    // Relay control is independent of WiFi/setup state -- the
    // only thing that can refuse a relay command is relays_enabled being
    // false (see POST /relay-control), a user-requested off switch, not
    // a readiness check. If pi-relay-control-alpine itself isn't
    // running or reachable, that's not caught here at all -- it just
    // surfaces naturally as every attempt reporting state "unknown"
    // (see relay_control.hpp's send_command).
    //
    // Forwards on/off to whichever relay pi-relay-control-alpine has
    // listening on that TCP port (see relay_control.hpp) and returns the
    // refreshed relay list either way -- including on failure (state
    // comes back "unknown"), so the response reflects reality rather
    // than optimistically assuming the write worked.
    auto do_relay = [&](int port, const std::string& action) -> bool {
        if (!relays_enabled.load()) {
            std::cerr << "[Relay] ignoring relay command: relay control is disabled\n";
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

    // The browser-based web UI itself -- a single self-contained static
    // page (see web_ui.hpp) that talks to the JSON routes below via
    // fetch(). This is now the only way to configure WiFi on this device
    // (see this file's own header comment) -- reachable from any browser
    // on whatever network connectivity the device already has (eth0,
    // preconfigured by the SD-card image, or an already-joined WiFi
    // network).
    server.route("GET", "/", [](const httpsrv::Request&) {
        return httpsrv::Response{200, webui::INDEX_HTML, "text/html; charset=utf-8", ""};
    });

    server.route("GET", "/status", [&](const httpsrv::Request& req) {
        std::string relays_str = relays_json(relays, relay_port_mu, relays_enabled.load());
        std::string scan_str;
        {
            std::lock_guard<std::mutex> lk(scan_mu);
            scan_str = last_scan_json;
        }
        bool must_change_password = req.user == "root" && !root_password_changed();
        std::ostringstream o;
        o << "{\"wifi\":" << status_json(wifi.get_status()) << ","
          << "\"relays\":" << relays_str << ","
          << "\"relaysEnabled\":" << (relays_enabled.load() ? "true" : "false") << ","
          << "\"victron\":" << victron_json(victronctl::query_status(victron_ctrl_port)) << ","
          << "\"scan\":" << scan_str << ","
          << "\"accounts\":" << accounts_json(list_accounts()) << ","
          << "\"sshEnabled\":" << (sshd_enabled() ? "true" : "false") << ","
          << "\"loggedInAs\":\"" << escape_json(req.user) << "\","
          << "\"mustChangePassword\":" << (must_change_password ? "true" : "false") << ","
          << "\"user\":" << camera_user_json() << "}";
        return httpsrv::Response::json(o.str());
    });

    server.route("POST", "/scan", [&](const httpsrv::Request&) {
        std::thread([&]() { InflightGuard guard; do_scan(); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    // Purely informational -- see CAMERA_USER_FILE's own comment. A
    // client labels this device with whoever's using it; name and/or
    // email may be empty.
    //
    // No route in this daemon reboots the device anymore (see do_forget's
    // own comment on why that was dropped) -- every write under /etc,
    // this one included, needs its own explicit commit or it would only
    // ever land in tmpfs and vanish on the next reboot regardless of
    // where that reboot actually comes from (a future /finish or
    // /forget don't touch this file at all, so they wouldn't save it for
    // free). `lbu` doesn't exist on non-diskless installs; run_command()
    // fails safely there (a normal execvp()+_exit(127), not an
    // exception), so this is safe to call unconditionally.
    server.route("POST", "/user", [&](const httpsrv::Request& req) {
        std::string name = json_get_string(req.body, "name");
        std::string email = json_get_string(req.body, "email");
        if (name.empty() && email.empty()) return httpsrv::Response::error(400, "name or email is required");
        write_camera_user(name, email);
        // -d: see do_forget's own comment on this flag -- load-bearing
        // here too, for the same reason.
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Command] user set: " << (name.empty() ? "(no name)" : name)
                   << (email.empty() ? "" : " <" + email + ">") << "\n";
        std::cerr << "[Main] lbu commit after user update: " << trim(commit.output) << "\n";
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/connect", [&](const httpsrv::Request& req) {
        std::string ssid = json_get_string(req.body, "ssid");
        std::string psk = json_get_string(req.body, "password");
        if (ssid.empty()) return httpsrv::Response::error(400, "ssid is required");
        std::cerr << "[Command] connect requested: \"" << ssid << "\"\n";

        // Always joins directly and synchronously in the background --
        // see this file's own header comment for why there's no more
        // "stage now, join later" distinction: wlan0's radio state can't
        // disrupt however this request reached the daemon (eth0 or an
        // already-joined WiFi network), so a live join can always be
        // attempted immediately. Poll GET /status's wifi.state for the
        // outcome.
        std::thread([&, ssid, psk]() { InflightGuard guard; do_connect(ssid, psk); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("POST", "/forget", [&](const httpsrv::Request&) {
        std::cerr << "[Command] forget requested\n";
        std::thread([&]() { InflightGuard guard; do_forget(); }).detach();
        return httpsrv::Response::json("{\"ok\":true}");
    });

    server.route("GET", "/accounts", [&](const httpsrv::Request&) {
        return httpsrv::Response::json(accounts_json(list_accounts()));
    });

    // Username and password are both supplied by the caller directly --
    // unlike the old single auto-generated admin account, there's no
    // credential-reveal step needed here at all, since whoever's filling
    // in the web UI's form already knows the password they just typed.
    server.route("POST", "/accounts", [&](const httpsrv::Request& req) {
        std::string user = json_get_string(req.body, "username");
        std::string pass = json_get_string(req.body, "password");
        if (user.empty() || pass.empty()) {
            return httpsrv::Response::error(400, "username and password are required");
        }
        std::cerr << "[Command] account add requested: " << user << "\n";
        std::string err;
        if (!add_account(user, pass, err)) {
            return httpsrv::Response::error(400, err);
        }
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after account add: " << trim(commit.output) << "\n";
        std::ostringstream o;
        o << "{\"ok\":true,\"accounts\":" << accounts_json(list_accounts()) << "}";
        return httpsrv::Response::json(o.str());
    });

    server.route("POST", "/accounts/remove", [&](const httpsrv::Request& req) {
        std::string user = json_get_string(req.body, "username");
        if (user.empty()) return httpsrv::Response::error(400, "username is required");
        std::cerr << "[Command] account remove requested: " << user << "\n";
        std::string err;
        if (!remove_account(user, err)) {
            return httpsrv::Response::error(400, err);
        }
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after account remove: " << trim(commit.output) << "\n";
        std::ostringstream o;
        o << "{\"ok\":true,\"accounts\":" << accounts_json(list_accounts()) << "}";
        return httpsrv::Response::json(o.str());
    });

    server.route("POST", "/ssh", [&](const httpsrv::Request& req) {
        bool enabled = json_get_bool(req.body, "enabled", true);
        std::cerr << "[Command] ssh " << (enabled ? "enable" : "disable") << " requested\n";
        if (!do_set_ssh_enabled(enabled)) {
            return httpsrv::Response::error(500, std::string("failed to ") + (enabled ? "enable" : "disable") + " sshd");
        }
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after ssh toggle: " << trim(commit.output) << "\n";
        return httpsrv::Response::json("{\"ok\":true}");
    });

    // "Change your own password," not an admin reset of someone else's:
    // req.user (set by the auth checker from the request's own Basic
    // Auth credentials -- see set_auth_checker() below) must match the
    // account being changed, checked here before ever looking at
    // currentPassword/newPassword, so knowing another account's current
    // password isn't enough on its own to change it out from under them
    // without also being logged in as them. The main reason this route
    // exists at all, though, is root: see ROOT_PASSWORD_CHANGED_FILE's
    // own comment -- this is the only way that gate's forced-change
    // screen (see the web UI's own JS) is ever satisfied.
    server.route("POST", "/change-password", [&](const httpsrv::Request& req) {
        std::string user = json_get_string(req.body, "username");
        std::string current_password = json_get_string(req.body, "currentPassword");
        std::string new_password = json_get_string(req.body, "newPassword");
        if (user.empty() || current_password.empty() || new_password.empty()) {
            return httpsrv::Response::error(400, "username, currentPassword and newPassword are required");
        }
        if (user != req.user) {
            return httpsrv::Response::error(403, "you can only change your own password");
        }
        std::cerr << "[Command] change-password requested for \"" << user << "\"\n";
        std::string err;
        if (!change_password(user, current_password, new_password, err)) {
            return httpsrv::Response::error(400, err);
        }
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after change-password: " << trim(commit.output) << "\n";
        return httpsrv::Response::json("{\"ok\":true}");
    });

    // No state in which setting the clock should be refused -- see
    // do_set_time's own comment.
    server.route("POST", "/time", [&](const httpsrv::Request& req) {
        long unix_time = json_get_int(req.body, "unixTime", -1);
        if (unix_time < 0) return httpsrv::Response::error(400, "unixTime (seconds since epoch, UTC) is required");
        std::cerr << "[Command] set_time requested: " << unix_time << "\n";
        if (!do_set_time(unix_time)) {
            return httpsrv::Response::error(500, "failed to set system time");
        }
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
          << "\"relays\":" << relays_json(relays, relay_port_mu, relays_enabled.load()) << "}";
        return httpsrv::Response::json(o.str());
    });

    // The master on/off switch for relay control -- distinct from
    // POST /relay's own per-relay on/off. Always callable, since
    // flipping this preference doesn't itself touch any relay, so
    // there's no readiness precondition to enforce here the way there is
    // for do_relay. Persists to config.ini immediately (see
    // set_relays_enabled) so a reboot doesn't revert a web UI change
    // back to whatever was last saved by hand -- the file and the
    // running process's own relays_enabled flag are kept in sync from
    // here on, in both directions.
    server.route("POST", "/relay-control", [&](const httpsrv::Request& req) {
        bool enabled = json_get_bool(req.body, "enabled", true);
        std::string err;
        if (!relayctl::set_relays_enabled(cfg_path, enabled, err)) {
            std::cerr << "[Relay] failed to persist enabled=" << enabled << ": " << err << "\n";
            return httpsrv::Response::error(500, err);
        }
        relays_enabled = enabled;
        std::cerr << "[Command] relay control " << (enabled ? "enabled" : "disabled") << "\n";
        auto commit = run_command({"lbu", "commit", "-d", "mmcblk0p1"});
        std::cerr << "[Main] lbu commit after relay-control: " << trim(commit.output) << "\n";
        return httpsrv::Response::json("{\"ok\":true}");
    });

    // Edits pi-relay-control-alpine's own config file directly (see
    // relay_control.hpp's own header comment for why always_on has no
    // live TCP command at all) and restarts it so the new value actually
    // takes effect -- that daemon only reads always_on once, at its own
    // startup. Rejects an unconfigured port up front, same reasoning as
    // do_relay's own check, before ever touching a file both daemons
    // share responsibility for staying in sync on.
    server.route("POST", "/relay-always-on", [&](const httpsrv::Request& req) {
        long port = json_get_int(req.body, "port", -1);
        bool always_on = json_get_bool(req.body, "alwaysOn", false);
        if (port < 0) return httpsrv::Response::error(400, "port is required");
        bool configured = std::any_of(relays.begin(), relays.end(),
                                       [&](const relayctl::RelayConfig& r) { return r.port == static_cast<int>(port); });
        if (!configured) return httpsrv::Response::error(400, "unconfigured port");

        std::cerr << "[Command] relay " << port << " always_on=" << always_on << " requested\n";
        std::string err;
        if (!relayctl::set_relay_always_on(static_cast<int>(port), always_on, err)) {
            std::cerr << "[Relay] failed to set always_on: " << err << "\n";
            return httpsrv::Response::error(500, err);
        }
        auto restart = run_command({"rc-service", "pi-relay-control", "restart"}, 15);
        if (restart.exit_code != 0) {
            std::cerr << "[Relay] pi-relay-control failed to restart after always_on change: "
                       << trim(restart.output) << "\n";
        }
        std::ostringstream o;
        o << "{\"ok\":true,\"relays\":" << relays_json(relays, relay_port_mu, relays_enabled.load()) << "}";
        return httpsrv::Response::json(o.str());
    });

    // Gates every route (including GET / itself) behind HTTP Basic Auth,
    // checked against this device's own real /etc/shadow -- see
    // auth.hpp's own header comment for why there's no separate
    // credential store to keep in sync. A browser's native Basic Auth
    // prompt is the entire "login page": no custom login form/session
    // handling needed in web_ui.hpp, consistent with this server's
    // otherwise stateless, no-keep-alive design.
    //
    // Layered on top of plain credential checking: root logging in with
    // its still-unchanged ROOT_PASSWORD is forced to change it before
    // anything else works (403 on every route except the handful that
    // have to stay reachable for that screen itself to function --
    // GET / to load the page, GET /status so its JS can even learn
    // mustChangePassword is true, and POST /change-password to actually
    // fix it). No equivalent gate for any other account: they're always
    // created with a password someone chose fresh (see add_account()),
    // never a shared/default one.
    server.set_auth_checker([](httpsrv::Request& req) -> int {
        if (!authctl::check_basic_auth(req.authorization, req.user)) return 401;
        if (req.user == "root" && !root_password_changed() &&
            req.path != "/" && req.path != "/status" && req.path != "/change-password") {
            return 403;
        }
        return 0;
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
    server.stop();

    return 0;
}
