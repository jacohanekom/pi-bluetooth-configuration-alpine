# pi-bluetooth-configuration-alpine

Configure a Raspberry Pi 3's WiFi over Bluetooth LE -- no SSH, no keyboard,
no display. A phone or app pairs with the Pi over BLE, writes an SSID and
passphrase, writes `connect`, and watches a status characteristic for the
result. For Raspberry Pi 3 running Alpine Linux (aarch64).

Built directly on [BlueZ](http://www.bluez.org/)'s D-Bus GATT-peripheral
API (`org.bluez.GattManager1` / `GattService1` / `GattCharacteristic1` /
`LEAdvertisement1` / `Agent1`) using plain `libdbus` -- no GLib, no
sd-bus/systemd dependency. WiFi itself is driven through `wpa_cli`
(wpa_supplicant's control interface) and `dhcpcd`.

## Raspberry Pi 3 prerequisite: attach the onboard Bluetooth UART

The Pi 3's Bluetooth chip (BCM43438) is wired to the SoC over UART, not
USB, and Alpine does **not** bring it up as `hci0` automatically. Before
this daemon (or `bluetoothd`) can do anything, you need to:

1. Make sure `/dev/ttyAMA0` isn't in use as a serial console (disable it
   in `/boot/cmdline.txt` / `/boot/config.txt` if it is).
2. Have the Broadcom firmware blob (`BCM43430A1.hcd`) in
   `/lib/firmware/brcm/` -- Alpine's Raspberry Pi firmware packages
   include this.
3. Attach the controller to the UART, e.g. `hciattach /dev/ttyAMA0 bcm43xx
   921600 flow -`, and make it persistent (Alpine's `/etc/mdev.conf` has a
   commented-out rule for this -- uncomment the "rpi bluetooth" line).

Full, current instructions: the
[Alpine wiki's Raspberry Pi 3 Bluetooth page](https://wiki.alpinelinux.org/wiki/Raspberry_Pi_3_-_Setting_Up_Bluetooth).
This is a one-time platform setup step, independent of this package --
until `hci0` exists, this daemon will keep failing to power on the
adapter and `supervise-daemon` will just keep respawning it every 5s
(harmless, just check `rc-service pi-bluetooth-configuration status` /
the log if it never reports advertising).

## How it verifies

CI (GitHub Actions) compiles this against Alpine's real `dbus-dev` and
`openssl-dev` headers on every push, which catches D-Bus API misuse and
build breakage. It cannot exercise the actual runtime behavior, though --
there's no Bluetooth radio, no `bluetoothd`, and no WiFi interface in a
GitHub Actions container. **The BLE pairing/GATT flow and the WiFi join
itself have only been verified by code review, not on real hardware.**
Test on an actual Pi 3 before relying on this for unattended provisioning.

## Security model

WiFi credentials cross the air twice: BLE (phone → Pi) and then the WiFi
radio itself (Pi → AP, already encrypted by WPA2). This daemon protects
the BLE leg by requiring **pairing** before any characteristic can be
read or written -- BlueZ enforces this at the ATT layer via the
`encrypt-read` / `encrypt-write` characteristic flags, refusing access
and triggering pairing automatically if the link isn't already encrypted.

Pairing uses the `NoInputNoOutput` I/O capability ("Just Works"), because
a headless Pi has no display or keyboard to confirm a passkey with. This
daemon's pairing agent auto-accepts every pairing request. **Just Works
encrypts the link against passive eavesdropping but provides no
protection against an active attacker during the initial pairing
handshake** (no PIN, no numeric comparison) -- the same trade-off most
headless BLE-provisioned IoT devices make. If that's not acceptable for
your environment, only pair the Pi in a physically controlled setting, or
adapt `bluetooth.agent_capability` / the agent's `RequestConfirmation`
handling in `src/gatt_server.hpp` for a display/PIN-based flow.

SSID and password bytes never pass through a shell: `wifi_control.hpp`
hands `wpa_cli` hex-encoded SSIDs and a PSK it derives itself
(PBKDF2-HMAC-SHA1, the same derivation WPA2 uses internally), so there is
nothing for a hostile SSID or passphrase to inject or break out of. One
caveat: because the PSK is derived from the raw passphrase bytes
locally, a WiFi password containing bytes not valid as text isn't an
issue -- but SSIDs are always sent as hex too, sidestepping wpa_supplicant's
control-interface quoting rules entirely.

## GATT service

Custom 128-bit UUIDs (no existing SIG profile fits this):

| Characteristic | UUID | Properties | Contents |
|---|---|---|---|
| Service | `7b1e0000-6a45-4d1f-9b0a-3c2f8e4d5a10` | -- | -- |
| SSID | `7b1e0001-6a45-4d1f-9b0a-3c2f8e4d5a10` | write, encrypted | UTF-8 network name |
| Password | `7b1e0002-6a45-4d1f-9b0a-3c2f8e4d5a10` | write, encrypted | UTF-8 passphrase (omit/empty for open networks) |
| Command | `7b1e0003-6a45-4d1f-9b0a-3c2f8e4d5a10` | write, encrypted | ASCII: `scan` \| `connect` \| `forget` |
| Status | `7b1e0004-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify, encrypted | JSON, see below |
| ScanResults | `7b1e0005-6a45-4d1f-9b0a-3c2f8e4d5a10` | read + notify, encrypted | JSON array, see below |

SSID/Password are write-only (no read) so a paired-but-different client
can't fetch back a passphrase someone else staged.

### Protocol

1. Pair with the Pi (BLE pairing, "Just Works" -- your BLE stack/app will
   prompt to confirm, with no PIN to enter).
2. Optionally write `scan` to Command, wait ~5s, then read (or subscribe
   to notifications on) ScanResults for a picklist.
3. Write the network name to SSID, the passphrase to Password (skip this
   write entirely for an open network), then write `connect` to Command.
4. Subscribe to Status notifications (or poll by reading it) until
   `state` is `connected` or `failed`.

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
agent_capability = NoInputNoOutput

[wifi]
interface        = wlan0

[scan]
max_results      = 10
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
- **No LE Secure Connections / MITM protection** (see Security model
  above) -- `NoInputNoOutput` is the only realistic option without adding
  a display or physical button to the Pi.
