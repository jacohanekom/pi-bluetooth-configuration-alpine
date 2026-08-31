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

This is a one-shot provisioning flow, not a managed session. A
successful `connect` creates `/.successfully-initialized` and reboots
the Pi a few seconds later; a `forget` removes that file and reboots the
same way. There is no ongoing management interface beyond BLE -- once
WiFi is set up (or torn down), the Pi reboots into its normal role
rather than staying up to be managed further. See "One-shot
provisioning and reboot behavior" below.

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
  non-empty `ip`): creates `/.successfully-initialized` (an empty marker
  file at the filesystem root), waits 3 seconds (enough time for that
  final `Status` notification to actually reach the BLE client before
  the connection drops), then reboots the Pi.
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

Separate from WiFi provisioning: `set_ethernet` (write an IPv4 address
to `EthernetIP`, then write `set_ethernet` to `Command`) gives `eth0` a
fixed static IP and starts a DHCP server (`dnsmasq`) scoped strictly to
`eth0`, so a laptop plugged directly into the Pi's ethernet port gets an
address automatically -- no router, no manual IP configuration on the
other end. `clear_ethernet` reverts `eth0` to normal DHCP client
behaviour and stops the DHCP server. Reading `EthernetIP` at any time
returns whatever IP is actually live on `eth0` right now (static or
DHCP-assigned).

This exists because plugging a WiFi-configured Pi into the same LAN over
Ethernet at the same time as testing WiFi can produce exactly the kind
of dual-homed routing confusion (asymmetric routing / `rp_filter`
silently dropping replies) that motivated this feature -- giving `eth0`
its own dedicated, isolated subnet sidesteps that entirely, and doubles
as a "plug in directly with a laptop" recovery path if WiFi is ever
misconfigured.

Unlike WiFi, this doesn't reboot the Pi: Ethernet doesn't share the
Pi 3's antenna with Bluetooth, so there's no coexistence problem forcing
a clean restart, and the change (`dhcpcd`/`dnsmasq` restarted directly)
takes effect within a couple of seconds.

**Safety**: `dnsmasq` is configured with `interface=eth0` and
`bind-interfaces` specifically so it only ever answers DHCP requests on
`eth0` -- it must never be allowed to also serve WiFi/upstream LAN
traffic, which would hand out conflicting addresses on a network this
daemon doesn't own. If you inspect or hand-edit
`/etc/dnsmasq.conf`/`/etc/dhcpcd.conf`, the daemon's own config lives in
a `# BEGIN pi-bluetooth-configuration eth0 static` / `# END ...`
delimited block that's rewritten idempotently on every `set_ethernet`/
`clear_ethernet` -- anything outside that block is left untouched.

## GATT service

Custom 128-bit UUIDs (no existing SIG profile fits this):

| Characteristic | UUID | Properties | Contents |
|---|---|---|---|
| Service | `7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10` | -- | -- |
| SSID | `7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 network name |
| Password | `7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 passphrase (omit/empty for open networks) |
| Command | `7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | ASCII: `scan` \| `connect` \| `forget` \| `set_ethernet` \| `clear_ethernet` |
| Status | `7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON, see below |
| ScanResults | `7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON array, see below |
| EthernetIP | `7b1e0006-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + write + notify | ASCII dotted-quad IPv4 address, see "Ethernet direct-connect" |

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
   `state` is `connected` or `failed`. On success, expect the Pi to
   reboot (and BLE to disconnect) a few seconds later -- see "One-shot
   provisioning and reboot behavior" below.

### Status JSON

```json
{"state":"connected","ssid":"MyWifi","ip":"192.168.1.42","error":""}
```

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
`dhcpcd`, and `dnsmasq` (plus their OpenRC services) automatically.
`dnsmasq` is only started when `set_ethernet` is actually used -- no
need to enable it manually.

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
apk add dbus dbus-openrc bluez bluez-openrc wpa_supplicant wpa_supplicant-openrc dhcpcd dhcpcd-openrc iproute2 dnsmasq dnsmasq-openrc

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
apk add build-base dbus-dev openssl-dev pkgconf

make
sudo make install               # installs to /usr/bin, /etc, /etc/init.d
```

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

[scan]
max_results      = 10
```

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
