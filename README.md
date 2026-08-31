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

The Pi 3's onboard Bluetooth chip shares a single antenna with its WiFi
radio, so BLE connections routinely drop once WiFi is actively passing
traffic (a hardware limitation, not a bug -- see "WiFi/Bluetooth
coexistence" below). BLE is therefore only relied on for the one thing
it has to do: get credentials across before any network exists. Once
the Pi has an IP, this daemon also exposes a plain TCP/IP control
interface on the LAN, and a well-behaved client hands off to that
instead of continuing to fight the radio for airtime.

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

## WiFi/Bluetooth coexistence and the TCP handoff

The Raspberry Pi 3's Bluetooth chip (BCM43438) shares a single
antenna/RF front-end with its WiFi radio. This is fine while WiFi is
idle, but once it's actively associated and passing traffic, it
routinely starves and drops BLE connections -- this is a well-documented
hardware limitation of this exact chip, not something specific to
BlueZ, Alpine, or any particular BLE central/OS.

So: don't fight it. Use BLE only for what it has to do (handing
credentials to a Pi with no network yet), and once `Status` reports a
non-empty `ip`, switch to the always-on TCP interface below for
everything else -- status polling, re-scanning, `forget`. It's the same
commands, just over IP instead of GATT, and isn't subject to the
antenna-sharing problem at all. A BLE disconnect right after `Status`
first reports `connected` is expected, not an error.

## TCP control interface

Listens on `network.port` (default `8567`), all interfaces, from
startup. One JSON object per line in each direction; the connection
stays open across multiple commands:

| Request | Reply |
|---|---|
| `{"cmd":"status"}` | `{"state":...,"ssid":...,"ip":...,"error":...}` |
| `{"cmd":"scan"}` | `{"ok":true}` (async, like the BLE Command char) |
| `{"cmd":"scanresults"}` | `[{"ssid":...,"rssi":...,"security":...}, ...]` |
| `{"cmd":"connect","ssid":"...","psk":"..."}` | `{"ok":true}` |
| `{"cmd":"forget"}` | `{"ok":true}` |

```bash
echo '{"cmd":"status"}' | nc 192.168.1.42 8567
```

**Unauthenticated**, like this project family's other TCP control ports
(`mp3-player`, `victron-ve-direct`) and like the BLE service itself --
anyone who can reach this port on the LAN can read status or reconfigure
WiFi credentials. Reasonable for a single-purpose home/lab Pi; not for a
shared or untrusted network.

## GATT service

Custom 128-bit UUIDs (no existing SIG profile fits this):

| Characteristic | UUID | Properties | Contents |
|---|---|---|---|
| Service | `7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10` | -- | -- |
| SSID | `7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 network name |
| Password | `7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | UTF-8 passphrase (omit/empty for open networks) |
| Command | `7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10` | write | ASCII: `scan` \| `connect` \| `forget` |
| Status | `7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON, see below |
| ScanResults | `7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify | JSON array, see below |

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
   `state` is `connected` or `failed`.
5. Once `state` is `connected` and `ip` is non-empty, switch to the TCP
   control interface at that `ip` for anything further -- see "WiFi/Bluetooth
   coexistence and the TCP handoff" below.

### Status JSON

```json
{"state":"connected","ssid":"MyWifi","ip":"192.168.1.42","error":""}
```

`state` is one of `idle`, `scanning`, `connecting`, `connected`, `failed`.

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
cleanly with `apk`, pulling in `dbus`, `bluez`, `wpa_supplicant`, and
`dhcpcd` (plus their OpenRC services) automatically.

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
apk add dbus dbus-openrc bluez bluez-openrc wpa_supplicant wpa_supplicant-openrc dhcpcd dhcpcd-openrc iproute2

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

[scan]
max_results      = 10

[network]
port             = 8567
```

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

- **Single active network.** `connect` replaces whatever network this
  daemon previously configured -- it's not a saved-network list manager.
- **Scan is a fixed 4s sleep-then-fetch**, not an event-driven wait for
  `CTRL-EVENT-SCAN-RESULTS`. Simple and reliable, if not instant.
- **No authentication or encryption at all, on either interface** -- see
  Security model above. Both BLE and the TCP control interface are wide
  open to anyone who can reach them.
