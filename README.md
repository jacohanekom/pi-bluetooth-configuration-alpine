# pi-bluetooth-configuration-alpine

Configure a Raspberry Pi 3's WiFi over Bluetooth LE -- no SSH, no keyboard,
no display. A phone or app connects to the Pi over BLE (no pairing --
see Security model below), writes an SSID and passphrase, writes
`connect`, and watches a status characteristic for the result. For
Raspberry Pi 3 running Alpine Linux (aarch64).

Built directly on [BlueZ](http://www.bluez.org/)'s D-Bus GATT-peripheral
API (`org.bluez.GattManager1` / `GattService1` / `GattCharacteristic1` /
`LEAdvertisement1`) using plain `libdbus` -- no GLib, no sd-bus/systemd
dependency. WiFi itself is driven through `wpa_cli` (wpa_supplicant's
control interface) and `dhcpcd`.

This is a one-shot provisioning flow, not a managed session. `connect`
itself doesn't reboot -- it leaves room for one more step, customizing
`eth0`'s local network configuration, before a `finish` command creates
`/.successfully-initialized` and reboots the Pi a few seconds later; a
`forget` removes that file and reboots the same way. There is no
ongoing management interface beyond BLE -- once WiFi is set up (or torn
down), the Pi reboots into its normal role rather than staying up to be
managed further. See "One-shot provisioning and reboot behavior" below.

## Raspberry Pi 3 prerequisite: dbus and bluetooth must be running

The Pi 3's Bluetooth chip (BCM43438) is wired to the SoC over UART, not
USB. On current Alpine `linux-rpi` kernels this needs **no manual
`hciattach`/`btattach` step** -- the kernel's own `hci_uart_bcm` serdev
driver binds to it and loads firmware automatically at boot (look for
`hci_uart_bcm serial0-0: ...` in `dmesg`). Older guides (including a
previous revision of this section, and the
[Alpine wiki's Raspberry Pi 3 Bluetooth page](https://wiki.alpinelinux.org/wiki/Raspberry_Pi_3_-_Setting_Up_Bluetooth))
describe a manual `hciattach`/`btattach` dance against `/dev/ttyAMA0`;
on the kernel this was verified against, that tty didn't even exist
under that name (the PL011 enumerated as `ttyAMA1`) and manual attach
wasn't needed at all, so try without it first.

What this daemon (and `bluetoothctl`) actually needs running first is
`dbus` and `bluetooth`:

```sh
rc-update add dbus default
rc-service dbus start
rc-update add bluetooth default
rc-service bluetooth start

bluetoothctl list   # should print "Controller <mac> ... [default]"
```

If `dbus` isn't running, `bluetoothctl` doesn't just fail cleanly -- it
aborts with an assertion inside BlueZ's D-Bus wrapper
(`dbus_connection_get_object_path_data(): assertion "connection != NULL"
failed`), which looks alarming but just means "no system bus to connect
to". Starting `dbus` first fixes it.

This is a one-time platform setup step, independent of this package --
until `hci0` exists and `dbus`/`bluetooth` are both up, this daemon will
keep failing to power on the adapter and `supervise-daemon` will just
keep respawning it every 5s (harmless, just check `rc-service
pi-bluetooth-configuration status` / the log if it never reports
advertising). If `bluetoothctl list` shows no controller even with both
services running, then check `dmesg | grep -iE 'uart|pl011|hci'` for
`hci_uart_bcm` load failures, and fall back to the manual `hciattach`/
`btattach` steps on the Alpine wiki page linked above.

## How it verifies

CI (GitHub Actions) compiles this against Alpine's real `dbus-dev` and
`openssl-dev` headers on every push, which catches D-Bus API misuse and
build breakage. It cannot exercise the actual runtime behavior, though --
there's no Bluetooth radio, no `bluetoothd`, and no WiFi interface in a
GitHub Actions container. **The BLE GATT flow and the WiFi join itself
have only been verified by code review and on one physical Pi 3, not
broadly.** Test on your own hardware before relying on this for
unattended provisioning.

## Security model

**No pairing, no encryption on BLE.** WiFi credentials (SSID and
passphrase) cross the BLE link in the clear to any device that connects
during the provisioning window -- there is no authentication or
encryption gate at all on this GATT service. This is a deliberate
trade-off, not an oversight: an earlier revision required BLE
pairing/bonding ("Just Works", since a headless Pi has no display or
keyboard to confirm a passkey with), but bonding on the Pi 3's BCM43438
proved unreliable enough in practice -- bond state going out of sync
between BlueZ and the central after either side's pairing record was
reset, manifesting as repeated OS-level "Connection Request" prompts and
CoreBluetooth's `peerRemovedPairingInformation` error -- that it wasn't
worth what "Just Works" pairing actually protected against in the first
place (which was already only passive eavesdropping, not an active
attacker, per the same trade-off most headless BLE-provisioned IoT
devices make).

Practical implication: **only use this on a trusted home/lab network,
during a provisioning window you control.** Anyone with a BLE-capable
device in range during that window can read the Pi's current WiFi status
or push new credentials to it. If you need real access control, put the
Pi somewhere physically private while provisioning, or don't leave
`pi-bluetooth-configuration` running/advertising outside of the moments
you're actively using it.

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
out of the way:

- **`connect` succeeds** (`Status` reports `state=connected` with a
  non-empty `ip`): does *not* reboot by itself -- `EthernetIP` stays
  writable for one more optional step (customizing `eth0`'s local
  network configuration) before the client sends `finish`.
- **`finish`**: only takes effect if WiFi is actually connected (a
  no-op, logged, otherwise). Creates `/.successfully-initialized` (an
  empty marker file at the filesystem root), waits 3 seconds (enough
  time for that final `Status` notification to actually reach the BLE
  client before the connection drops), then reboots the Pi. This is
  what actually concludes the wizard.
- **`forget`**: removes `/.successfully-initialized` if present, then
  reboots the same way (also after the 3-second delay).

`/.successfully-initialized` is meant for other boot-time scripts/units
on the Pi to check (`test -f /.successfully-initialized`) to know
whether WiFi provisioning has ever completed successfully -- this daemon
itself doesn't read it back.

The reboot is a plain `reboot` (no shell, via the same argv-array
`run_command` helper used for `wpa_cli`/`dhcpcd`), which goes through
OpenRC's normal shutdown sequence. Expect the BLE connection (and this
daemon along with it) to disappear a few seconds after either action --
that's expected, not a crash.

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
which includes the window right after WiFi connects but before `finish`
is sent, the wizard's "local network configuration" step: write
`"<ip>,<rangeStart>,<rangeEnd>"` (e.g. `"192.168.4.1,2,200"`) to
`EthernetIP`, then write `set_ethernet` to `Command`, to replace the
gateway IP and DHCP range with new ones. **Once `finish` actually runs,
the daemon rejects further `set_ethernet` writes** (logged, no-op) --
at that point Ethernet's job is done being reconfigurable, so its
config is left alone. Reading `EthernetIP` at any time returns the same
`"ip,rangeStart,rangeEnd"` format reflecting whatever's actually live
on `eth0` right now, regardless of which state you're in.

This exists because plugging a WiFi-configured Pi into the same LAN over
Ethernet at the same time as testing WiFi can produce exactly the kind
of dual-homed routing confusion (asymmetric routing / `rp_filter`
silently dropping replies) that motivated this feature -- giving `eth0`
its own dedicated, isolated subnet sidesteps that entirely, and doubles
as a "plug in directly with a laptop" recovery path if WiFi is ever
misconfigured.

Unlike WiFi, applying/changing this doesn't reboot the Pi: Ethernet
doesn't share the Pi 3's antenna with Bluetooth, so there's no
coexistence problem forcing a clean restart, and the change
(`dhcpcd`/`dnsmasq` restarted directly) takes effect within a couple of
seconds.

The chosen IP and DHCP range persist in a plain state file
(`/etc/pi-bluetooth-configuration/eth0-static-ip`, `"ip,rangeStart,rangeEnd"`)
so they survive reboots (`ip addr add` on its own doesn't);
`denyinterfaces` is persisted the same way as the rest of this
feature's config, as a `# BEGIN pi-bluetooth-configuration eth0 static`
/ `# END ...` delimited block inside `/etc/dhcpcd.conf`.

**Allocated IPs**: `DhcpLeases` (read + notify) reports whatever's
currently in dnsmasq's own leases file
(`/var/lib/misc/dnsmasq.leases`) as JSON --
`[{"ip":...,"mac":...,"hostname":...}, ...]` -- so the app can show
which devices are actually plugged into `eth0` right now. A background
thread polls that file every 5 seconds and only notifies when it
actually changes.

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
letting the same BLE app that provisions WiFi also flip relays on the
Pi -- no separate BLE service or app needed for something as simple as
turning a light or a fan on and off.

This daemon doesn't drive GPIO itself and has no idea what's wired to
which pin -- it's a thin TCP client that forwards `on`/`off`/`status` to
whichever port pi-relay-control-alpine has that relay listening on
(`127.0.0.1:<port>`, the same protocol `nc` uses -- see that repo's
README), and reports the result back over BLE.

Relay control is no part of the provisioning wizard -- it only works
once setup has actually finished (`finish` has run and
`/.successfully-initialized` exists). Before that, `relay <port> on|off`
is rejected (logged, no-op) and `Relays` reports an empty list, without
even querying pi-relay-control-alpine: that daemon's own `start_pre()`
gate (see its README, "Requires device provisioning") means nothing is
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
(or omit it) if pi-relay-control-alpine isn't installed -- the `Relays`
characteristic then just reports an empty list.

**Protocol**: once setup has finished, write `relay <port> on` or
`relay <port> off` to `Command`, then read (or subscribe to
notifications on) `Relays` for the result -- see "Relays JSON" below. A
background thread also polls every configured relay's state every 5
seconds (again, only once setup has finished) and notifies on change,
so a connected client sees relays toggled from elsewhere (another
client, `always_on` resuming after pi-relay-control-alpine restarts,
etc.) without having to write anything itself first.

**Failure handling**: if pi-relay-control-alpine isn't running, isn't
installed, or the configured port doesn't match its config, a `relay`
command or a `Relays` read just reports that relay's `state` as
`unknown` (logged on the daemon's side) -- it never blocks or crashes
the BLE service over it. Each TCP round-trip is capped at a 2-second
timeout, since it's loopback traffic that should be near-instant if the
other daemon is actually up.

## GATT service

Custom 128-bit UUIDs (no existing SIG profile fits this):

| Characteristic | UUID | Properties | Contents |
|---|---|---|---|
| Service | `7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10` | -- | -- |
| SSID | `7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 network name |
| Password | `7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 passphrase (omit/empty for open networks) |
| Command | `7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | ASCII: `scan` \| `connect` \| `forget` \| `set_ethernet` \| `finish` \| `relay <port> on\|off` (`relay` only takes effect after `finish` runs) |
| Status | `7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON, see below |
| ScanResults | `7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON array, see below |
| EthernetIP | `7b1e0006-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + write + notify | ASCII `"ip,rangeStart,rangeEnd"`, see "Ethernet direct-connect" (write only takes effect before `finish` runs) |
| DhcpLeases | `7b1e0007-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON array, see "Ethernet direct-connect" |
| Relays | `7b1e0008-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON array, see "Relay control" (reports `[]` until `finish` has run) |

No pairing/encryption gates any of these -- see Security model above.
SSID/Password are still write-only (no read) purely so a second BLE
client can't fetch back a passphrase the first one staged; it's not a
security boundary against a client that's actually listening in on the
writes themselves.

### Protocol

1. Connect to the Pi over BLE (no pairing step -- connecting is enough).
2. Optionally write `scan` to Command, wait ~5s, then read (or subscribe
   to notifications on) ScanResults for a picklist.
3. Write the network name to SSID, the passphrase to Password (skip this
   write entirely for an open network), then write `connect` to Command.
4. Subscribe to Status notifications (or poll by reading it) until
   `state` is `connected` or `failed`. A successful connect does *not*
   reboot the Pi by itself.
5. Optionally customize the local network: write
   `"<ip>,<rangeStart>,<rangeEnd>"` to EthernetIP, then write
   `set_ethernet` to Command (see "Ethernet direct-connect").
6. Write `finish` to Command. This is what actually concludes setup:
   expect the Pi to reboot (and BLE to disconnect) a few seconds later
   -- see "One-shot provisioning and reboot behavior" below.

### Status JSON

```json
{"state":"connected","ssid":"MyWifi","ip":"192.168.1.42","error":"","finished":false}
```

`finished` reflects whether `/.successfully-initialized` exists --
i.e. whether a previous `finish` already completed. A client should use
this, not just `state`, to decide whether to show the setup wizard or
the final read-only details screen: a Pi that's mid-wizard (WiFi just
joined, `finish` not sent yet) also reports `state:"connected"`.

`state` is one of `idle`, `scanning`, `connecting`, `connected`, `failed`.

This reflects wpa_supplicant's actual live state. Rather than checking
just once at startup (which can race wpa_supplicant still finishing a
reconnect and cache a stale `idle` forever after), every read lazily
re-queries `wpa_cli status` for as long as this process hasn't yet
tracked a definite state of its own -- so if WiFi was already connected
before this daemon started (e.g. the Pi just rebooted and wpa_supplicant
reconnected on its own), `Status` correctly reports `connected` on the
first read, however long after startup that read happens to occur. Once
this process performs its own `connect`/`forget`, that takes over and
the live re-check stops, so it never overwrites an in-progress
`connecting` state with something stale from wpa_supplicant mid-change.

### ScanResults JSON

```json
[{"ssid":"MyWifi","rssi":-52,"security":"WPA2"},
 {"ssid":"Neighbour","rssi":-81,"security":"WPA2"}]
```

Sorted strongest-first, deduplicated by SSID, capped at
`scan.max_results` (default 10). Hidden networks (blank SSID in the scan)
are omitted since there's nothing to show for them.

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

### A note on message size

GATT characteristic values top out at 512 bytes (the spec limit), and
`ReadValue` honours BlueZ's `offset` option so long values are delivered
correctly via BlueZ's own read-blob mechanism -- no custom chunking
protocol needed on either side. `scan.max_results` defaults to 10 to stay
comfortably within that limit even with longer SSIDs.

## Install the .apk (recommended)

Every push builds `pi-bluetooth-configuration-aarch64.apk` (GitHub
Actions artifact; tagged `v*` pushes also attach it to a GitHub Release),
built via `abuild` from [`alpine/APKBUILD`](alpine/APKBUILD). It installs
cleanly with `apk`, pulling in `dbus`, `bluez`, `wpa_supplicant`,
`dhcpcd`, `dnsmasq`, and `iptables` (plus their OpenRC services, where
applicable) automatically. `dnsmasq` is started automatically the first
time the daemon applies `eth0`'s default gateway IP (see "Ethernet
direct-connect") -- no need to enable it manually. `iptables` needs no
service of its own; the daemon applies its NAT rules itself at startup
(see "Internet sharing (eth0 -> WiFi)").

It's signed with a throwaway key generated fresh in CI each run (there's
no distributed repo to establish trust for), so install with
`--allow-untrusted`:

```sh
apk add --allow-untrusted ./pi-bluetooth-configuration-aarch64.apk

rc-update add dbus default
rc-update add bluetooth default
rc-update add wpa_supplicant default
rc-update add pi-bluetooth-configuration default

rc-service dbus start
rc-service bluetooth start
rc-service wpa_supplicant start
rc-service pi-bluetooth-configuration start
```

Uninstall with `apk del pi-bluetooth-configuration`.

## Install from the release tarball

Every push builds `pi-bluetooth-configuration-alpine-aarch64.tar.gz`
(GitHub Actions artifact; tagged `v*` pushes also attach it to a GitHub
Release).

```sh
apk add dbus dbus-openrc bluez bluez-openrc wpa_supplicant wpa_supplicant-openrc dhcpcd dhcpcd-openrc iproute2 dnsmasq dnsmasq-openrc iptables

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
apk add build-base dbus-dev openssl-dev pkgconf iptables

make
sudo make install               # installs to /usr/bin, /etc, /etc/init.d
```

`iptables` is a runtime dependency, not a build one -- listed here too
since a from-source build doesn't otherwise pull it in automatically the
way the `.apk` does.

## Configuration

Edit `/etc/pi-bluetooth-configuration/config.ini`:

```ini
[bluetooth]
adapter          = hci0
device_name      = pi-bluetooth-configuration

[wifi]
interface        = wlan0

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
```

`[relays]` is optional -- see "Relay control" above for the format and
what it integrates with.

`device_name` is only a fallback. The advertised BLE name is normally
the board's own hardware serial number (read from `/proc/cpuinfo` at
startup), not this configured string -- so a client's device list shows
which physical Pi is which instead of the same name for every unit.
`device_name` is used as-is only when a serial can't be read (e.g. not
running on real Pi hardware).

`wpa_supplicant` must already be running against the same interface with
a control socket (`ctrl_interface=/var/run/wpa_supplicant` and
`update_config=1` in `/etc/wpa_supplicant/wpa_supplicant.conf`) -- this
daemon talks to it via `wpa_cli`, it doesn't start or own
`wpa_supplicant` itself.

Restart after changes: `rc-service pi-bluetooth-configuration restart`

## Service management

```bash
rc-service pi-bluetooth-configuration start
rc-service pi-bluetooth-configuration stop
rc-service pi-bluetooth-configuration restart
rc-update add pi-bluetooth-configuration default   # start on boot
rc-service pi-bluetooth-configuration status
```

Runs as root (it needs to reconfigure the adapter and WiFi), respawns
automatically on failure (5s delay, unlimited retries, via
`supervise-daemon`), and logs to `/var/log/pi-bluetooth-configuration.log`.

## Known limitations (v1)

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
  above. The BLE service is wide open to anyone who can reach it.
- **Reboots unconditionally on success/forget**, with no way to opt out
  short of editing `src/main.cpp`. If you need the daemon to stay up
  afterward for some other purpose, remove the `reboot_after_delay()`
  calls in `do_connect`/`do_forget`.
