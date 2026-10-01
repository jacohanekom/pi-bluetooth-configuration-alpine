#pragma once
/**
 * auth.hpp -- HTTP Basic Auth for http_server.hpp, checked against this
 * device's own real Unix accounts in /etc/shadow -- not a separate
 * credential store of any kind. "Which accounts can log into the web
 * UI" and "which accounts exist on this device" (root, plus anything
 * POST /accounts in main.cpp created) are therefore always exactly the
 * same set, by construction -- there is nothing here to keep in sync by
 * hand, and nothing that could drift.
 *
 * Password verification shells out to `openssl passwd -6 -salt <salt>`
 * rather than linking libcrypt/calling crypt(3) directly, deliberately
 * mirroring how this project already creates these same hashes (see
 * main.cpp's add_account() and build-image.sh's own ROOT_PASSWORD
 * handling, both already "shell out to openssl rather than reimplement
 * crypt(3)"): extracting the salt from the stored hash and re-running
 * the same `openssl passwd -6` that produced it is byte-for-byte
 * deterministic, so a matching password reproduces the identical
 * "$6$salt$hash" string. Only $6$ (SHA-512 crypt) hashes are accepted --
 * the only format this project's own tooling ever produces -- so there
 * is no legacy-algorithm downgrade path to worry about.
 */
#include <fstream>
#include <string>
#include <vector>

#include "subprocess.hpp"

namespace authctl {

inline std::string trim_ws(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Reads /etc/shadow directly (root-only readable; this daemon always
// runs as root) rather than going through getspnam()/PAM -- consistent
// with this project's existing style of plain file parsing over heavier
// system APIs elsewhere. Returns the hash field (2nd colon-separated
// column) for the given user, or "" if not found -- an empty/invalid
// hash can never match in verify_password() below, so "no such user"
// and "found but locked" both safely fail closed the same way.
inline std::string shadow_hash(const std::string& user) {
    std::ifstream f("/etc/shadow");
    std::string line;
    while (std::getline(f, line)) {
        auto c1 = line.find(':');
        if (c1 == std::string::npos) continue;
        if (line.compare(0, c1, user) != 0) continue;
        auto c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        return line.substr(c1 + 1, c2 - c1 - 1);
    }
    return "";
}

// Verifies a plaintext password against this device's own /etc/shadow.
// Only ever matches a "$6$<salt>$<hash>" field -- a locked account
// ("!", "*", or empty) or any other hash format is rejected outright,
// before ever shelling out. The password is passed to `openssl passwd`
// as a plain argv element, not stdin, for the same reason
// add_account() already does: execvp has no shell to leak it through,
// and this device has no untrusted local users who could read another
// root process's /proc/<pid>/cmdline during the sub-second window this
// runs.
inline bool verify_password(const std::string& user, const std::string& password) {
    std::string hash = shadow_hash(user);
    if (hash.size() < 4 || hash[0] != '$') return false;

    auto p1 = hash.find('$', 1);
    if (p1 == std::string::npos) return false;
    auto p2 = hash.find('$', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string algo = hash.substr(1, p1 - 1);
    if (algo != "6") return false; // only SHA-512 crypt -- see this file's own header comment

    std::string salt = hash.substr(p1 + 1, p2 - p1 - 1);
    auto computed = run_command({"openssl", "passwd", "-6", "-salt", salt, password});
    if (computed.exit_code != 0) return false;
    return trim_ws(computed.output) == hash;
}

// Plain base64 decode (RFC 4648, standard alphabet) -- just enough for
// decoding a "Basic <base64>" Authorization header's payload; invalid
// characters (padding, whitespace) are skipped rather than erroring,
// since a malformed header should just fail the credential check below
// rather than needing its own distinct error path.
inline std::string base64_decode(const std::string& in) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int table[256];
    for (int& t : table) t = -1;
    for (size_t i = 0; i < alphabet.size(); ++i) table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);

    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] == -1) continue;
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// `authorization_header` is the raw value of an Authorization header --
// e.g. "Basic cm9vdDpodW50ZXIy" -- or empty if none was sent. See
// http_server.hpp's own Request::authorization field. On success,
// `out_user` is set to whichever account just authenticated -- main.cpp
// needs this to know *who* is logged in (e.g. to gate on "root hasn't
// changed their password yet"), not just "someone valid did".
inline bool check_basic_auth(const std::string& authorization_header, std::string& out_user) {
    const std::string prefix = "Basic ";
    if (authorization_header.size() <= prefix.size()) return false;
    if (authorization_header.compare(0, prefix.size(), prefix) != 0) return false;

    std::string decoded = base64_decode(authorization_header.substr(prefix.size()));
    auto colon = decoded.find(':');
    if (colon == std::string::npos) return false;

    std::string user = decoded.substr(0, colon);
    std::string pass = decoded.substr(colon + 1);
    if (!verify_password(user, pass)) return false;
    out_user = user;
    return true;
}

} // namespace authctl
