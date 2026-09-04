# pi-bluetooth-configuration-alpine

Configure a Raspberry Pi 3's WiFi using only its own onboard WiFi radio --
no SSH, no keyboard, no display, no second board, no Bluetooth. On
startup this daemon tries to join whatever network is already
configured; if that fails -- including the common case of nothing being
configured yet -- it switches the radio into its own access point that a
phone can join directly, reaching a plain HTTP/JSON API to submit real
credentials. This is the same shape ESP8266/ESP32 "WiFiManager"-style
devices use for headless setup. For Raspberry Pi 3 running Alpine Linux
(aarch64).

WiFi station mode is driven through `wpa_cli` (wpa_supplicant's control
interface) and `dhcpcd`; the fallback access point is driven through
`hostapd` plus a dedicated `dnsmasq` DHCP scope. The HTTP API itself is a
minimal, dependency-free server over plain POSIX sockets (no framework),
thread-per-connection. No Bluetooth, no D-Bus, no BlueZ anywhere in this
design -- see git history if you're looking for the earlier BLE-based
revision this replaced.

This is a one-shot provisioning flow, not a managed session. Submitting
credentials while the fallback AP is active necessarily ends that AP (the
radio can't run station and AP mode at once), so a successful join in
that case immediately creates `/.successfully-initialized` and reboots
the Pi a few seconds later; a `forget` removes that file and reboots the
same way. There is no ongoing management interface beyond this same HTTP
API -- once WiFi is set up (or torn down), the Pi reboots into its normal
role rather than staying up to be managed further. See "One-shot
provisioning and reboot behavior" below.

## How it verifies

CI (GitHub Actions) compiles this against Alpine's real `openssl-dev`
headers on every push, which catches build breakage, and separately
reproduces the exact `.apk` build (via `abuild`/`alpine/APKBUILD`),
which additionally catches missing runtime dependencies (like
`hostapd`/`hostapd-openrc`) and packaging mistakes. Neither can exercise
the actual runtime behavior, though -- there's no WiFi radio, no
`hostapd`, and no real network interface in a GitHub Actions container.
**The AP-fallback flow and the WiFi join itself have only been verified
by code review and on one physical Pi 3, not broadly.** Test on your own
hardware before relying on this for unattended provisioning.

## Security model

**No authentication, no encryption, on either the fallback AP or the
HTTP API.** The AP itself is open (no password), and every HTTP request
is plain unauthenticated HTTP -- WiFi credentials cross both the AP's own
air interface and this API in the clear to anyone in range during the
provisioning window. This is a deliberate trade-off, not an oversight:
requiring a password just to reach the setup flow that hands out the
real network's password in the clear anyway wouldn't add meaningful
protection, just friction, and TLS with no sensible way to provision a
trusted certificate onto a headless device buys little over plain HTTP
here either.

Practical implication: **only use this on a trusted home/lab network,
during a provisioning window you control.** Anyone in WiFi range during
that window can join the fallback AP, read the Pi's current status, or
push new credentials to it. If you need real access control, put the Pi
somewhere physically private while provisioning, or don't leave it in
fallback-AP mode outside of the moments you're actively using it (it
only enters that mode automatically when it can't join a configured
network, so this mainly means: configure it promptly).

SSID and password bytes never pass through a shell: `wifi_control.hpp`
hands `wpa_cli` hex-encoded SSIDs and a PSK it derives itself
(PBKDF2-HMAC-SHA1, the same derivation WPA2 uses internally), so there is
nothing for a hostile SSID or passphrase to inject or break out of. One
caveat: because the PSK is derived from the raw passphrase bytes
locally, a WiFi password containing bytes not valid as text isn't an
issue -- but SSIDs are always sent as hex too, sidestepping wpa_supplicant's
control-interface quoting rules entirely.

## One-shot provisioning and reboot behavior

This daemon isn't meant to stay up managing an active WiFi connection --
its only job is to get the Pi onto a network (or off one) and then get
out of the way. `POST /connect`'s exact behavior depends on whether the
fallback AP is currently active, because of a hard hardware constraint:
this radio can't run AP and station mode at once, so submitting real
credentials while the AP is active necessarily severs the phone's own
connection to this daemon (reached via the AP) the moment the radio
switches over -- there's no way to keep serving that same phone a
"did it work" answer afterward.

- **If the fallback AP is active** (fresh setup, or the previously
  configured network couldn't be joined): `POST /connect` acknowledges
  the request immediately, then on a background thread tears the AP
  down and attempts to join the given network. On success, this
  immediately creates `/.successfully-initialized` and reboots -- there's
  no separate `finish` step in this path, since by the time the outcome
  is known the AP (and the phone's only path to this daemon) is already
  gone. On failure, the AP is restarted so the phone has something to
  reconnect to and retry against.
- **If the AP is *not* active** (already on a real network, e.g.
  reconfiguring): `POST /connect` joins the given network without
  marking setup finished or rebooting -- `POST /ethernet` stays available
  for one more optional step (customizing `eth0`'s local network
  configuration) before the client sends `POST /finish`.
- **`POST /finish`**: only takes effect if WiFi is actually connected (a
  no-op, logged, otherwise). Creates `/.successfully-initialized` (an
  empty marker file at the filesystem root), waits 3 seconds (enough
  time for the HTTP response to actually reach the client before the
  connection drops), then reboots the Pi.
- **`POST /forget`**: removes `/.successfully-initialized` if present,
  then reboots the same way (also after the 3-second delay).

`/.successfully-initialized` is meant for other boot-time scripts/units
on the Pi to check (`test -f /.successfully-initialized`) to know
whether WiFi provisioning has ever completed successfully -- this daemon
itself doesn't read it back.

The reboot is a plain `reboot` (no shell, via the same argv-array
`run_command` helper used for `wpa_cli`/`dhcpcd`), which goes through
OpenRC's normal shutdown sequence. Expect the HTTP connection (and this
daemon along with it) to disappear a few seconds after either action --
that's expected, not a crash. If the phone was on the fallback AP when
this happened, it will need to rejoin its regular WiFi network (or the
newly-configured one, once the Pi finishes rebooting onto it) to reach
the Pi again.

## Ethernet direct-connect

`eth0` is meant to always be a working gateway, not something you have
to configure before it's useful: on every boot, the daemon assigns it a
static IP directly (`ip addr add`) -- whatever was last chosen, or a
default of `192.168.4.1` (overridable via `ethernet.ip` in
`config.ini`) if nothing has been chosen yet -- and starts a combined
DHCP+DNS server (`dnsmasq`) scoped strictly to `eth0`, so a laptop
plugged directly into the Pi's ethernet port gets an address *and
working DNS* automatically, no router, no manual configuration on the
other end, no app interaction required.

The address is assigned directly rather than through dhcpcd's own
static-IP config, and deliberately so: dhcpcd only applies its config
once it detects carrier on the interface, so a Pi sitting with nothing
plugged into `eth0` would show no address at all (`ip addr show eth0`
reporting `NO-CARRIER` with no `inet` line) and only pick one up some
moments after a cable is inserted. A gateway address needs to be there
*before* anything is plugged in, so a client is served the instant it
connects -- so this daemon assigns it directly and tells dhcpcd to
leave `eth0` alone entirely (`denyinterfaces`), removing the carrier
dependency altogether.

This is customizable for as long as the setup wizard hasn't finished --
which includes the whole time the fallback AP is active, and (when it
wasn't needed) the window right after WiFi connects but before `POST
/finish` is sent: `POST /ethernet` with `{"ip":...,"rangeStart":...,
"rangeEnd":...}` (e.g. `{"ip":"192.168.4.1","rangeStart":2,"rangeEnd":200}`)
replaces the gateway IP and DHCP range with new ones. **Once setup
actually finishes, the daemon rejects further `POST /ethernet` calls**
(logged, no-op) -- at that point Ethernet's job is done being
reconfigurable, so its config is left alone. `GET /ethernet` (or the
`eth` field of `GET /status`) at any time returns
`{"ip":...,"rangeStart":...,"rangeEnd":...}` reflecting whatever's
actually live on `eth0` right now, regardless of which state you're in.

This exists because plugging a WiFi-configured Pi into the same LAN over
Ethernet at the same time as testing WiFi can produce exactly the kind
of dual-homed routing confusion (asymmetric routing / `rp_filter`
silently dropping replies) that motivated this feature -- giving `eth0`
its own dedicated, isolated subnet sidesteps that entirely, and doubles
as a "plug in directly with a laptop" recovery path if WiFi is ever
misconfigured.

Unlike a WiFi network change, applying/changing this doesn't reboot the
Pi: eth0 is entirely independent of whatever wlan0 is doing, so there's
no coexistence problem forcing a clean restart, and the change
(`dhcpcd`/`dnsmasq` restarted directly) takes effect within a couple of
seconds. WiFi's own connect/forget/finish flows reboot by deliberate
design choice, not hardware necessity -- see "One-shot provisioning and
reboot behavior" above.

The chosen IP and DHCP range persist in a plain state file
(`/etc/pi-bluetooth-configuration/eth0-static-ip`, `"ip,rangeStart,rangeEnd"`)
so they survive reboots (`ip addr add` on its own doesn't);
`denyinterfaces` is persisted the same way as the rest of this
feature's config, as a `# BEGIN pi-bluetooth-configuration eth0 static`
/ `# END ...` delimited block inside `/etc/dhcpcd.conf`.

**Allocated IPs**: the `leases` field of `GET /status` reports whatever's
currently in dnsmasq's own leases file
(`/var/lib/misc/dnsmasq.leases`) as JSON --
`[{"ip":...,"mac":...,"hostname":...}, ...]` -- so the app can show
which devices are actually plugged into `eth0` right now, read fresh on
every poll.

**DNS**: `dnsmasq` also answers DNS queries from `eth0` clients (not
just DHCP), forwarding them upstream using whatever nameservers are in
`/etc/resolv.conf` -- normally whatever WiFi's own DHCP handed
`dhcpcd`. The DHCP lease explicitly points clients at this Pi
(`dhcp-option=option:dns-server,<gateway ip>`) for DNS. Without this, a
device on `eth0` gets an address and (via "Internet sharing" below) a
route to the internet, but domain names don't resolve -- exactly the
symptom this fixes.

**Safety**: `dnsmasq` is configured with `interface=eth0` and
`bind-interfaces` specifically so it only ever answers DHCP *and DNS*
requests on `eth0` -- it must never be allowed to also serve WiFi/
upstream LAN traffic, which would hand out conflicting addresses (DHCP)
or expose an open resolver (DNS) on a network this daemon doesn't own.
If you inspect or hand-edit `/etc/dnsmasq.conf`/`/etc/dhcpcd.conf`, the
daemon's own config lives in a `# BEGIN pi-bluetooth-configuration eth0
static` / `# END ...` delimited block that's rewritten idempotently on
every `set_ethernet` call -- anything outside that block is left
untouched.

**Internet sharing**: a device plugged into `eth0` gets real internet
access, not just a link to the Pi -- the daemon enables IPv4 forwarding
and NATs (`iptables`/`MASQUERADE`) `eth0`'s traffic out through the WiFi
interface, which is what actually holds the internet connection here.
Applied once at startup, right after `eth0`'s static IP -- see
"Internet sharing (eth0 -> WiFi)" below for details.

## Internet sharing (eth0 -> WiFi)

`eth0` is a dead-end network of its own (see "Ethernet direct-connect"
above) unless something routes its traffic somewhere with actual
internet access -- which, on this Pi, is WiFi. At startup, right after
applying `eth0`'s static IP, the daemon:

1. Enables `net.ipv4.ip_forward` (both live, via `/proc/sys/...`, and
   persisted for the next boot via a drop-in in `/etc/sysctl.d/`).
2. Adds `iptables` rules NATing traffic from `eth0` out through the WiFi
   interface (`iptables -t nat -A POSTROUTING -o <wifi_iface> -j
   MASQUERADE`) plus the matching `FORWARD` rules to actually let that
   traffic across between the two interfaces.

This isn't gated by WiFi's own connection state or the setup wizard --
the rules reference the WiFi interface by name and are harmless to have
in place even before it's associated to anything; they simply have
nothing to NAT through until it is. Idempotent: each rule is checked
(`iptables -C`) before being added, so restarting the daemon (or
rebooting) doesn't pile up duplicate rules.

**Requires the `iptables` package** (a runtime dependency of the `.apk`
-- see below; install it yourself if building from source or the
tarball) **and a kernel with netfilter NAT support** (`iptable_nat`,
`nf_nat`, `nf_conntrack` -- built into Alpine's `linux-rpi` kernel).

**Known limitation**: there's no way to disable this short of editing
`eth_control.hpp` (`enable_internet_sharing`) -- like the rest of
`eth0`'s "always a working gateway" behavior, it's unconditional. If you
don't want `eth0` devices reaching the internet through this Pi, don't
plug anything into it, or remove the resulting `iptables` rules
yourself.

## Relay control

A separate, optional integration with
[pi-relay-control-alpine](https://github.com/jacohanekom/pi-relay-control-alpine),
letting the same app that provisions WiFi also flip relays on the
Pi -- no separate app or protocol needed for something as simple as
turning a light or a fan on and off.

This daemon doesn't drive GPIO itself and has no idea what's wired to
which pin -- it's a thin TCP client that forwards `on`/`off`/`status` to
whichever port pi-relay-control-alpine has that relay listening on
(`127.0.0.1:<port>`, the same protocol `nc` uses -- see that repo's
README), and reports the result back over HTTP.

Relay control is no part of the provisioning wizard -- it only works
once setup has actually finished (`/.successfully-initialized` exists).
Before that, `POST /relay` is rejected (logged, no-op, `ok:false`) and
the `relays` field of `GET /status` reports an empty list, without even
querying pi-relay-control-alpine: that daemon's own `start_pre()` gate
(see its README, "Requires device provisioning") means nothing is
listening on those ports yet anyway. This mirrors where the client app
surfaces the feature too -- alongside WiFi/network stats on the
post-setup details screen, not as a step in the wizard.

**Setup**: install and configure
[pi-relay-control-alpine](https://github.com/jacohanekom/pi-relay-control-alpine)
separately (it owns the actual GPIO pins and TCP ports), then list the
same ports here under `[relays]` in `/etc/pi-bluetooth-configuration/config.ini`,
one line per relay:

```ini
[relays]
relay 7778 Camera Light
relay 7779 Fan
```

`<port>` must match a `relay <gpio_pin> <port> [...]` line in
pi-relay-control-alpine's own `/etc/pi-relay-control.conf` -- this file
only needs the port and a free-text display label, since GPIO wiring is
that other daemon's concern, not this one's. Restart after changes:
`rc-service pi-bluetooth-configuration restart`. Leave `[relays]` empty
(or omit it) if pi-relay-control-alpine isn't installed -- the `relays`
field of `GET /status` then just reports an empty list.

**Protocol**: once setup has finished, `POST /relay` with
`{"port":7778,"state":"on"}` (or `"off"`) -- the response includes the
freshly-queried `relays` array immediately, so the client gets an
authoritative update without waiting for its next `GET /status` poll --
see "Relays JSON" below. `GET /status` itself also queries every
configured relay's live state fresh on every call (no caching), so a
client polling it sees relays toggled from elsewhere (another client,
`always_on` resuming after pi-relay-control-alpine restarts, etc.)
without having to act itself first.

**Failure handling**: if pi-relay-control-alpine isn't running, isn't
installed, or the configured port doesn't match its config, a `relay`
command or a status query just reports that relay's `state` as
`unknown` (logged on the daemon's side) -- it never blocks or crashes
the HTTP server over it (each connection is its own thread -- see "HTTP
API" below). Each TCP round-trip is capped at a 2-second timeout, since
it's loopback traffic that should be near-instant if the other daemon
is actually up.

## Victron solar/battery telemetry

Another separate, optional integration, this time with
[victron-ve-direct-alpine](https://github.com/jacohanekom/victron-ve-direct-alpine),
which reads a Victron device over its VE.Direct serial protocol. This
lets the same app that provisions WiFi also show the latest
solar/battery reading, no separate app or LAN access needed. This
integration targets MPPT solar chargers specifically -- see "Victron
JSON" below for the fields it does (and deliberately doesn't) carry.

This daemon doesn't talk VE.Direct itself -- it queries
victron-ve-direct-alpine's status control port (`echo status | nc
127.0.0.1 <port>`, the same one its own README documents) and republishes
the reply as JSON in the `victron` field of `GET /status`, using the same
field names as that project's own `data_port` telemetry frames (see its
README's "JSON output") so there's only one schema to learn across both
projects.

Unlike relay control, this is read-only -- there's no action to gate
behind setup finishing, just a reading to show or not. It's live from
the moment this daemon starts, regardless of wizard state; the app
simply chooses to display it on the same post-setup screen as
WiFi/network stats and relays, not because the daemon requires it.

**Setup**: install and configure
[victron-ve-direct-alpine](https://github.com/jacohanekom/victron-ve-direct-alpine)
separately (it owns the actual serial connection to the Victron device),
then point this daemon at its status port under `[victron]` in
`/etc/pi-bluetooth-configuration/config.ini`:

```ini
[victron]
ctrl_port  = 8562
```

`ctrl_port` must match the `[output] ctrl_port` value in
victron-ve-direct-alpine's own `config.ini` -- `8562` is that project's
own default, so the two match out of the box if neither is customized.
Restart after changes: `rc-service pi-bluetooth-configuration restart`.

**Protocol**: read the `victron` field of `GET /status` -- see "Victron
JSON" below. Queried fresh on every poll (no caching), so a client's
Solar/Battery view updates on its own as it keeps polling.

**Failure handling**: if victron-ve-direct-alpine isn't running, isn't
installed, the configured port doesn't match its config, or it's up but
hasn't synced a frame from the VE.Direct device yet, this just reports
`{"connected":false}` -- it never blocks or crashes the HTTP server over
it. Each TCP round-trip is capped at a 2-second timeout, same as relay
control.

## HTTP API

Plain JSON over HTTP/1.1, no authentication (see Security model above).
Every response closes the connection (no keep-alive); a request body, if
any, must be a flat JSON object -- no nesting, matching exactly what
these routes need.

| Route | Body | Response |
|---|---|---|
| `GET /status` | -- | Combined snapshot: `wifi`, `apActive`, `eth`, `leases`, `relays`, `victron`, `scan` -- see "Status JSON" below. No server push (no BLE-style notify): poll this periodically instead. |
| `POST /scan` | -- | `{"ok":true}` immediately; triggers a background scan (~4s). Poll `GET /status`'s `scan` field for results. |
| `POST /connect` | `{"ssid":...,"password":...}` (omit/empty password for an open network) | `{"ok":true}` immediately; see "One-shot provisioning and reboot behavior" above for what happens next, which differs depending on whether the fallback AP is currently active. |
| `POST /forget` | -- | `{"ok":true}` immediately; forgets the configured network and reboots a few seconds later. |
| `POST /finish` | -- | `{"ok":true}` immediately; only takes effect if WiFi is connected and the fallback AP isn't active (see above) -- concludes setup and reboots. |
| `GET /ethernet` | -- | `{"ip":...,"rangeStart":...,"rangeEnd":...}` -- eth0's current gateway config. |
| `POST /ethernet` | `{"ip":...,"rangeStart":...,"rangeEnd":...}` | `{"ok":true}`; see "Ethernet direct-connect" (rejected once setup has finished). |
| `POST /relay` | `{"port":...,"state":"on"\|"off"}` | `{"ok":bool,"relays":[...]}` -- see "Relay control" (rejected until setup has finished). |

### Protocol

1. Join the Pi's fallback AP (its own hardware serial as the SSID, no
   password) if it's advertising one, or otherwise reach the Pi on
   whatever network it's already on.
2. Optionally `POST /scan`, wait ~5s, then check `GET /status`'s `scan`
   field for a picklist.
3. `POST /connect` with the chosen `ssid`/`password`.
4. Poll `GET /status` until `wifi.state` is `connected` or `failed`.
   What happens next depends on `apActive` at the time `/connect` was
   called -- see "One-shot provisioning and reboot behavior" above. If
   the AP was active, expect to lose the connection to the Pi entirely
   at this point (the AP goes away as part of joining the real network) --
   there is no further polling to do from this same network path.
5. If the AP was *not* active (already on a real network, reconfiguring):
   optionally customize the local network with `POST /ethernet`, then
   `POST /finish` to conclude setup -- expect the Pi to reboot a few
   seconds later.

### Status JSON

`GET /status` returns:

```json
{
  "wifi": {"state":"connected","ssid":"MyWifi","ip":"192.168.1.42","error":"","finished":false},
  "apActive": false,
  "eth": {"ip":"192.168.4.1","rangeStart":2,"rangeEnd":200},
  "leases": [{"ip":"192.168.4.55","mac":"...","hostname":"laptop"}],
  "relays": [{"port":7778,"label":"Camera","state":"on"}],
  "victron": {"connected": false},
  "scan": [{"ssid":"MyWifi","rssi":-52,"security":"WPA2"}]
}
```

`wifi.finished` reflects whether `/.successfully-initialized` exists --
i.e. whether setup has already completed. A client should use this, not
just `wifi.state`, to decide whether to show the setup wizard or the
final read-only details screen: a Pi that's mid-wizard (WiFi just
joined, not finished yet) also reports `wifi.state:"connected"`.

`wifi.state` is one of `idle`, `scanning`, `connecting`, `connected`,
`failed`. This reflects wpa_supplicant's actual live state. Rather than
checking just once at startup (which can race wpa_supplicant still
finishing a reconnect and cache a stale `idle` forever after), every
read lazily re-queries `wpa_cli status` for as long as this process
hasn't yet tracked a definite state of its own -- so if WiFi was already
connected before this daemon started (e.g. the Pi just rebooted and
wpa_supplicant reconnected on its own), `wifi.state` correctly reports
`connected` on the first read, however long after startup that read
happens to occur. Once this process performs its own connect/forget,
that takes over and the live re-check stops, so it never overwrites an
in-progress `connecting` state with something stale from wpa_supplicant
mid-change.

`apActive` reflects whether the fallback access point is currently
running (see "One-shot provisioning and reboot behavior" above).

`scan` is sorted strongest-first, deduplicated by SSID, capped at
`scan.max_results` (default 10). Hidden networks (blank SSID in the scan)
are omitted since there's nothing to show for them. Starts as `[]` until
the first `POST /scan` completes.

`eth` and `leases` are documented in "Ethernet direct-connect" above,
`relays` in "Relay control", `victron` in "Victron solar/battery
telemetry" -- all computed fresh on every `GET /status` call (no
server-side caching), since a slow query only ever delays this one
request, not a shared dispatch thread (this server is
thread-per-connection).

### Relays JSON

```json
[{"port":7778,"label":"Camera Light","state":"on"},
 {"port":7779,"label":"Fan","state":"off"}]
```

One entry per relay configured in the `[relays]` section of
`config.ini` (see "Relay control" above), in the order they're listed
there. `state` is `on`, `off`, or `unknown` (pi-relay-control-alpine
isn't reachable on that port -- not installed, not running, or a
port/config mismatch between the two daemons). This is an empty array
(`[]`) until setup has actually finished, regardless of how many relays
are configured -- see "Relay control" above.

### Victron JSON

```json
{
  "connected": true,
  "device": {"pid": "0xA067", "name": "SmartSolar MPPT 100|50", "serial": "HQ2241A3JKL", "fw": "161"},
  "V": 12.540,
  "I": 0.150,
  "VPV": 18.200,
  "PPV": 4,
  "CS": 5,
  "CS_name": "Float",
  "ERR": 0,
  "ERR_name": "No error",
  "H20": 0.12
}
```

`connected: false` (with every other key absent) covers every failure
case -- victron-ve-direct-alpine not installed, not running, a
port/config mismatch, or up but hasn't synced a VE.Direct frame yet --
see "Victron solar/battery telemetry" above. Field names and units match
victron-ve-direct-alpine's own `data_port` JSON exactly -- see that
project's README, "JSON output" -- with two deliberate omissions:
`SOC`/`TTG` (state of charge, time to go), which only exist for battery
monitors (BMV-series), and `LOAD` (the charger's load output switch
state), which isn't present on every MPPT model and is always `"ON"` on
this integration's hardware -- neither is modeled here at all since
there's nothing informative in either for this integration's target
hardware.

## Install the .apk (recommended)

Every push builds `pi-bluetooth-configuration-aarch64.apk` (GitHub
Actions artifact; tagged `v*` pushes also attach it to a GitHub Release),
built via `abuild` from [`alpine/APKBUILD`](alpine/APKBUILD). It installs
cleanly with `apk`, pulling in `wpa_supplicant`, `dhcpcd`, `dnsmasq`,
`iptables`, and `hostapd` (plus their OpenRC services, where applicable)
automatically. `dnsmasq` is started automatically the first time the
daemon applies `eth0`'s default gateway IP (see "Ethernet
direct-connect") -- no need to enable it manually. `hostapd` is
deliberately *not* enabled at boot -- this daemon starts/stops it itself,
dynamically, as it enters/leaves fallback-AP mode (see "One-shot
provisioning and reboot behavior" above). `iptables` needs no service of
its own; the daemon applies its NAT rules itself at startup (see
"Internet sharing (eth0 -> WiFi)").

It's signed with a throwaway key generated fresh in CI each run (there's
no distributed repo to establish trust for), so install with
`--allow-untrusted`:

```sh
apk add --allow-untrusted ./pi-bluetooth-configuration-aarch64.apk

rc-update add wpa_supplicant default
rc-update add pi-bluetooth-configuration default

rc-service wpa_supplicant start
rc-service pi-bluetooth-configuration start
```

Uninstall with `apk del pi-bluetooth-configuration`.

## Install from the release tarball

Every push builds `pi-bluetooth-configuration-alpine-aarch64.tar.gz`
(GitHub Actions artifact; tagged `v*` pushes also attach it to a GitHub
Release).

```sh
apk add wpa_supplicant wpa_supplicant-openrc dhcpcd dhcpcd-openrc iproute2 dnsmasq dnsmasq-openrc iptables hostapd hostapd-openrc

tar xzf pi-bluetooth-configuration-alpine-aarch64.tar.gz
cd pi-bluetooth-configuration-alpine-aarch64

install -Dm755 pi-bluetooth-configuration /usr/bin/pi-bluetooth-configuration
install -Dm644 config.ini /etc/pi-bluetooth-configuration/config.ini
install -Dm755 pi-bluetooth-configuration.initd /etc/init.d/pi-bluetooth-configuration

rc-update add pi-bluetooth-configuration default
rc-service pi-bluetooth-configuration start
```

## Build from source

```sh
apk add build-base openssl-dev pkgconf iptables hostapd

make
sudo make install               # installs to /usr/bin, /etc, /etc/init.d
```

`iptables`/`hostapd` are runtime dependencies, not build ones -- listed
here too since a from-source build doesn't otherwise pull them in
automatically the way the `.apk` does.

## Configuration

Edit `/etc/pi-bluetooth-configuration/config.ini`:

```ini
[http]
port             = 8080

[wifi]
interface        = wlan0
device_name      = pi-bluetooth-configuration
connect_timeout_secs = 20

[ap]
ip               = 192.168.5.1
dhcp_range_start = 2
dhcp_range_end   = 200

[ethernet]
interface        = eth0
ip               = 192.168.4.1
dhcp_range_start = 2
dhcp_range_end   = 200

[scan]
max_results      = 10

[relays]
; relay 7778 Camera Light
; relay 7779 Fan

[victron]
ctrl_port  = 8562
```

`[relays]` and `[victron]` are both optional -- see "Relay control" and
"Victron solar/battery telemetry" above for the formats and what they
integrate with.

`[wifi]`'s `device_name` is only a fallback. The fallback AP's SSID is
normally the board's own hardware serial number (read from
`/proc/cpuinfo` at startup), not this configured string -- so a client's
WiFi network list shows which physical Pi is which instead of the same
name for every unit. `device_name` is used as-is only when a serial
can't be read (e.g. not running on real Pi hardware).
`connect_timeout_secs` is how long to wait, on startup, for
wpa_supplicant to join whatever's already configured before giving up
and starting the fallback AP instead.

`[ap]`'s subnet is deliberately distinct from `[ethernet]`'s so the two
can never collide if a client happens to be on both eth0 and the
fallback AP at once.

`wpa_supplicant` must already be running against the same interface with
a control socket (`ctrl_interface=/var/run/wpa_supplicant` and
`update_config=1` in `/etc/wpa_supplicant/wpa_supplicant.conf`) -- this
daemon talks to it via `wpa_cli`, it doesn't start or own
`wpa_supplicant` itself (though it does briefly stop/start it around
entering/leaving fallback-AP mode -- see `ap_control.hpp`).

Restart after changes: `rc-service pi-bluetooth-configuration restart`

## Service management

```bash
rc-service pi-bluetooth-configuration start
rc-service pi-bluetooth-configuration stop
rc-service pi-bluetooth-configuration restart
rc-update add pi-bluetooth-configuration default   # start on boot
rc-service pi-bluetooth-configuration status
```

Runs as root (it needs to reconfigure the WiFi radio, run hostapd, and
edit network config files), respawns automatically on failure (5s delay,
unlimited retries, via `supervise-daemon`), and logs to
`/var/log/pi-bluetooth-configuration.log`.

## Known limitations (v1)

- **No AP+station concurrency.** This radio can only be in station mode
  or AP mode at once, never both -- a hard hardware/driver constraint,
  not a design choice. This is why `POST /connect` behaves differently
  depending on whether the fallback AP is active (see "One-shot
  provisioning and reboot behavior" above), and why a phone loses its
  connection to the Pi partway through submitting fresh credentials in
  that case.
- **Single active network.** `connect`/`forget` clear *every* network
  wpa_supplicant currently knows about (queried live via
  `wpa_cli list_networks`, not tracked in-process) before acting --
  this isn't a saved-network list manager. Querying live rather than
  remembering "the last id this process added" matters specifically
  because the daemon reboots the Pi after every successful connect/forget
  (see above), which restarts wpa_supplicant too; an in-process id would
  only ever know about networks added since the current process started,
  silently leaking a stale network into wpa_supplicant.conf on every
  cycle instead of replacing it.
- **Scan is a fixed 4s sleep-then-fetch**, not an event-driven wait for
  `CTRL-EVENT-SCAN-RESULTS`. Simple and reliable, if not instant.
- **No authentication or encryption at all** -- see Security model
  above. Both the fallback AP and the HTTP API are wide open to anyone
  who can reach them.
- **Reboots unconditionally on success/forget**, with no way to opt out
  short of editing `src/main.cpp`. If you need the daemon to stay up
  afterward for some other purpose, remove the `reboot_after_delay()`
  calls in `do_connect`/`do_forget`.
