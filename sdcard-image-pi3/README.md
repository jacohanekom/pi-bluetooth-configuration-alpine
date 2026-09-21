# sdcard-image-pi3

Builds a bootable SD card image for **Raspberry Pi 3 and other
64-bit-capable boards** (`aarch64`) with
[pi-bluetooth-configuration](..),
[pi-relay-control](https://github.com/jacohanekom/pi-relay-control-alpine),
and [victron-ve-direct](https://github.com/jacohanekom/victron-ve-direct-alpine)
pre-installed and enabled -- write it to a card, boot it, and all three
daemons are already running.

Unlike a Raspberry Pi OS image (or an earlier version of this same
image), this is **Alpine's own official diskless release**
(`alpine-rpi-*-aarch64.tar.gz`, genuinely tested by the Alpine project)
plus a bundled, fully-offline-capable local apk repository and a
pre-built `apkovl` config overlay -- not a disk-resident filesystem we
assembled from scratch ourselves. See "Why diskless, not disk-resident"
below for why.

## Why diskless, not disk-resident

An earlier version of this image built a normal disk-resident ("sys"-
style) root filesystem from scratch via `apk add` in a container, the
same way `alpine/APKBUILD` in each of the three repos does. Real
test-booting that on a Pi 3 surfaced multiple boot-sequence bugs, all
stemming from re-implementing parts of Alpine's own boot machinery
ourselves instead of using Alpine's already-correct version of it:

- `wpa_supplicant`'s OpenRC service checks for a wireless interface
  exactly once at start, with no retry -- the onboard SDIO WiFi driver
  measurably lags behind the rest of boot, so it routinely lost that
  race and never found `wlan0`.
- Root was mounted read-only for the entire boot (the kernel's default
  when nothing says otherwise) because we never enabled the
  `fsck`/`root`/`localmount` OpenRC services that check, remount, and
  mount things -- breaking every on-disk write, including the
  first-boot script's own SSH host key generation.
- Hostname showed as `(none)` at the login prompt for reasons that
  needed real boot-log access to fully diagnose.

Alpine's own diskless boot mode sidesteps this whole class of bug: root
is tmpfs, so there's no `fsck`/`root`/`localmount` step to get wrong at
all, and the hostname/getty/service-ordering logic is Alpine's own
well-tested `initramfs` `init` script, not ours. The trade-off is a
fully different config-persistence model -- see below.

## How it works

- **Boot files** (kernel, initramfs, `config.txt`, device tree blobs)
  come straight from Alpine's official `alpine-rpi-*-aarch64.tar.gz`
  release, unmodified.
- That release already bundles a small local apk repository
  (`apks/aarch64/` + a `.boot_repository` marker file) covering the
  base system and a handful of common daemons -- `build-image.sh`
  extends this same repository with everything our three daemons
  additionally need (`hostapd`, `dnsmasq`, `iptables`, `avahi`, `dbus`,
  WiFi firmware) plus the three daemons' own `.apk` files, then
  re-signs the index with a throwaway key generated fresh for the
  build.
- `nlplug-findfs` (the initramfs's own boot-media scanner, confirmed by
  reading its actual source) finds this repository via the
  `.boot_repository` marker at boot and adds it to
  `/etc/apk/repositories` -- **entirely offline**, no network needed,
  confirmed by running the exact `apk add --no-network` resolution the
  boot process performs and checking it actually succeeds before
  trusting it.
- A pre-built `<hostname>.apkovl.tar.gz` overlay (also just a plain
  tarball, built by `build-image.sh`) supplies `/etc/apk/world` (the
  package list to install every boot), the signing key, hostname,
  `/etc/runlevels/*` service-enablement symlinks, and a
  `/etc/local.d/aipicam-setup.start` script that hashes in the root
  password and fixes up `sshd_config` once the relevant packages are
  actually installed (they don't exist yet when the overlay itself is
  unpacked -- see the comments in `build-image.sh`).
- The whole thing is a **single FAT32 partition** (unlike the old
  two-partition boot+ext4 layout) -- diskless mode needs nowhere else
  to put anything.

## Config persistence across reboots

Diskless mode rebuilds the entire system from the local apk repo fresh
on **every** boot -- root is tmpfs, so by default nothing survives a
reboot at all. This image handles that with two different mechanisms
for two different classes of state:

- **`/etc` and the provisioning marker** (`/etc/successfully-initialized`
  -- `pi-relay-control`'s own `start_pre()` refuses to start without
  it) are committed back to the boot partition by `pi-bluetooth-
  configuration` itself, calling `lbu commit -d mmcblk0p1` (Alpine's own
  diskless config-persistence tool -- the same one `setup-alpine`'s
  interactive wizard would normally wire up for you) right before it
  reboots at the end of a successful setup (see main.cpp's
  `reboot_after_delay()`), and also right after `POST /user` (see that
  route). This is what makes **WiFi credentials** (written into
  `/etc/wpa_supplicant/wpa_supplicant.conf`), **SSH host keys**, and the
  provisioning marker all survive a reboot -- an earlier version tried
  this via a generic OpenRC shutdown-runlevel service instead, which
  turned out to fail on real hardware for reasons not fully root-caused;
  triggering it explicitly at the one moment the app actually knows a
  reboot is imminent is both simpler and more reliable.
  `-d` is load-bearing, not cosmetic: `lbu commit`'s own target filename
  is `$(hostname).apkovl.tar.gz`, computed from the *current* hostname
  at commit time (confirmed by reading `alpine-conf`'s own `lbu.in`) --
  but this daemon sets the live hostname to this device's hardware
  serial on every startup (see `set_hostname_from_serial()`), which no
  longer matches the image's build-time hostname (e.g. `aipicam`)
  already sitting on the boot partition. Without `-d`, `lbu commit`
  finds that mismatch and refuses outright ("more than one apkovl
  file(s) were found ... Please use -d to replace") rather than doing
  anything useful -- meaning every commit failed silently-to-the-user
  (though logged), losing WiFi credentials and the marker both.
  Reproduced exactly against real Alpine tooling (not just inferred
  from source) before fixing. `/etc/apk/protected_paths.d/lbu.list`
  (baked into the apkovl at build time) is what tells `lbu` which paths
  to track -- just `/etc` itself; the marker file lives *under* `/etc`
  specifically (not bare at the filesystem root, where it used to live)
  because `lbu commit` actually works via `apk audit --backup`, which
  reliably tracks new/changed files *within* a protected directory but
  -- confirmed directly with a real test, not assumed -- silently never
  picks up a bare top-level file no matter how it's listed in
  `lbu.list`. Found on real hardware: WiFi credentials correctly
  survived a reboot while the marker (still at the old bare-root path
  at the time) silently didn't.
- **Relay on/off state** (`pi-relay-control`'s "resume last position
  after reboot" feature) lives under `/var`, which isn't covered by
  the `/etc`-only `lbu` tracking above -- it's made to survive instead
  by symlinking `/var/lib/relay_control` to
  `/media/mmcblk0p1/relay-state`, a directory pre-created directly on
  the boot partition (the actual SD card content, mounted read-write
  for the whole time the system runs).

An **unclean** power loss (pulling power rather than the app's own
controlled reboot) skips the commit entirely -- whatever changed since
the last successful setup/forget cycle won't be captured. Not fixable
in general for a diskless system without a UPS or similar; just worth
knowing.

## Prerequisites

- Docker Desktop (used both for the aarch64 build -- genuinely native
  on Apple Silicon, no QEMU -- and the plain filesystem-assembly step;
  everything operates on plain image files via `mkfs.vfat`/`mtools`, no
  loop devices or privileged containers needed, so this also runs
  unmodified on macOS).
- `gh` (GitHub CLI), authenticated, to fetch the three `.apk` build
  artifacts.
- ~1GB free disk space, plus whatever Docker needs to cache the
  `alpine:3.22` image and the official Alpine RPi release download.

## 1. Fetch the three aarch64 `.apk` artifacts

```sh
mkdir -p artifacts

gh run list -R jacohanekom/pi-bluetooth-configuration-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-bluetooth-configuration-alpine -n pi-bluetooth-configuration-apk-aarch64 -D artifacts

gh run list -R jacohanekom/pi-relay-control-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-relay-control-alpine -n pi-relay-control-apk-aarch64 -D artifacts

gh run list -R jacohanekom/victron-ve-direct-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/victron-ve-direct-alpine -n victron-ve-direct-apk-aarch64 -D artifacts
```

or attach a tagged release's `.apk` files there directly. Either way
you should end up with:

```
artifacts/pi-bluetooth-configuration-aarch64.apk
artifacts/pi-relay-control-aarch64.apk
artifacts/victron-ve-direct-aarch64.apk
```

## 2. Build the image

```sh
ROOT_PASSWORD='something-you-choose' ./build-image.sh
```

Optional: `PI_HOSTNAME=whatever` (defaults to `aipicam`). Output is
`aipicam-pi3-diskless.img` (~768MB).

`ROOT_PASSWORD` is only ever used in-memory to compute a SHA-512 crypt
hash (`openssl passwd -6`) baked into the `apkovl`; the plaintext itself
is never written to disk or committed anywhere.

## Remote access via Cloudflare Tunnel (optional)

By default this Pi is only reachable over SSH while you're on the same
LAN (or its own fallback AP). If you also want to reach it from
anywhere -- e.g. it's deployed somewhere without you physically present
-- `build-image.sh` can bake in a
[Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/)
client (`cloudflared`) that makes an **outbound-only** connection to
Cloudflare's edge, entirely opt-in. Unlike a self-hosted VPN, there's
**no server for you to run or maintain** -- `cloudflared` only ever
makes outbound HTTPS connections out to Cloudflare, so it works behind
any NAT/firewall with no port forwarding anywhere, in either direction.
The tradeoff is routing SSH traffic through Cloudflare's infrastructure
instead of a box you control.

A Cloudflare Tunnel identifies *itself*, not a specific device -- unlike
SSH host keys, there's no automatic per-device identity. Two Pis
sharing one tunnel token would both register as connectors for the
*same* tunnel, with traffic load-balanced between them unpredictably --
not a hard error, just no way to address one specifically. There are
two ways to provide a token, matching the two situations that come up:

### Reusing an existing tunnel (rebuilding the same device)

```sh
ROOT_PASSWORD='something-you-choose' \
CLOUDFLARE_TUNNEL_TOKEN='<your tunnel token>' \
./build-image.sh
```

**One-time setup**, free Cloudflare account required:

1. [dash.teams.cloudflare.com](https://dash.teams.cloudflare.com) (or
   the "Zero Trust" section of the regular Cloudflare dashboard) ->
   **Networks -> Tunnels -> Create a tunnel** -> choose **Cloudflared**
   as the connector.
2. Name it (e.g. `aipicam-pi3`) and continue -- the next screen shows an
   install command containing a long token (`cloudflared service
   install <TOKEN>`). Copy just the `<TOKEN>` part; that's
   `CLOUDFLARE_TUNNEL_TOKEN` above.
3. Configure routing -- see "Reaching the Pi once connected" below.

### Provisioning a new tunnel automatically (a new device)

```sh
ROOT_PASSWORD='something-you-choose' \
CLOUDFLARE_API_TOKEN='<a Cloudflare API token>' \
CLOUDFLARE_ACCOUNT_ID='<your account ID>' \
./build-image.sh
```

`build-image.sh` calls the Cloudflare API to create a brand new tunnel
(named `aipicam-<hostname>-<timestamp>`) for this specific build and
bakes in its token automatically -- same reasoning as SSH host keys/the
WireGuard keypair being generated fresh per device, just done at build
time instead of on first boot, since a tunnel has no equivalent
first-boot self-provisioning of its own.

**One-time setup**: [dash.cloudflare.com/profile/api-tokens](https://dash.cloudflare.com/profile/api-tokens)
-> **Create Token** -> custom token with **Cloudflare Tunnel: Edit**
permission, scoped to your account. Find `CLOUDFLARE_ACCOUNT_ID` on the
right sidebar of any zone's Overview page in the regular Cloudflare
dashboard, or via `Account Home`.

After the build, the new tunnel exists but has **no routing configured
yet** -- see below, same one-time step as the manual path.

### Reaching the Pi once connected

Either way, the tunnel itself needs to be told how to route to this Pi,
once, in the dashboard -- easiest is **Private Network** routing (no
public hostname/domain needed at all): under the tunnel's own settings,
add a Private Network route covering whatever address you want to reach
it at (e.g. a `/32` for a single made-up address, since this Pi has no
fixed LAN IP of its own to route to). Then install the
[WARP client](https://developers.cloudflare.com/cloudflare-one/connections/connect-devices/warp/)
on whatever machine you'll SSH *from*, enrol it in the same Zero Trust
account, and enable Private Network routing there too.
`ssh root@<the address you routed>` then reaches the Pi through the
tunnel from anywhere your WARP client is connected.

(Alternative: a **Public Hostname** route instead, if you'd rather use a
domain you already have in Cloudflare -- point it at
`ssh://localhost:22`, then `cloudflared access ssh --hostname
<that-hostname>` from the client side instead of installing WARP. Either
works; Private Network avoids needing a domain at all.)

## 3. Write it to an SD card

**Double-check the device path before running `dd` -- writing to the
wrong disk destroys its contents with no warning and no undo.**

```sh
diskutil list                      # find your SD card, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=aipicam-pi3-diskless.img of=/dev/rdisk4 bs=4m status=progress
sync
diskutil eject /dev/disk4
```

(Use the `/dev/rdiskN` "raw" device, not `/dev/diskN`, for a much faster
write on macOS.)

## Building in CI instead of locally

[`sdcard-image-pi3.yml`](../.github/workflows/sdcard-image-pi3.yml) runs
the exact same script on a genuine aarch64 GitHub-hosted runner
(`ubuntu-24.04-arm`, no QEMU). `workflow_dispatch` only -- it depends on
all three repos' latest artifacts, not just this one, and produces a
build artifact you don't want piling up on every commit.

One-time setup, repo secrets on **pi-bluetooth-configuration-alpine**
(Settings -> Secrets and variables -> Actions):

- `SDCARD_ROOT_PASSWORD` -- same meaning as the local `ROOT_PASSWORD`
  env var above.
- `CROSS_REPO_GH_TOKEN` -- this repo's own `.apk` is fetched with the
  default `GITHUB_TOKEN`, but the other two repos' latest artifacts
  need a token that can read *their* Actions runs too. Mint a
  fine-grained PAT (https://github.com/settings/personal-access-tokens)
  scoped to just `pi-relay-control-alpine` and `victron-ve-direct-alpine`,
  with **Actions: Read-only** repository permission, and save it here.
- `SDCARD_CLOUDFLARE_TUNNEL_TOKEN`, or `SDCARD_CLOUDFLARE_API_TOKEN` +
  `SDCARD_CLOUDFLARE_ACCOUNT_ID` (all optional) -- same meaning as the
  matching local env vars above (the `SDCARD_` prefix is just to avoid
  colliding with Cloudflare's own commonly-used bare `CLOUDFLARE_*`
  secret names). Leave all unset to skip Cloudflare Tunnel entirely,
  same as locally. Secrets rather than `workflow_dispatch` inputs since
  these are real credentials, not a cosmetic setting like `hostname`.

Then trigger it from the Actions tab, or:

```sh
gh workflow run sdcard-image-pi3.yml -R jacohanekom/pi-bluetooth-configuration-alpine
```

Grab the result from the run's Artifacts section
(`aipicam-pi3-diskless`, kept 14 days).

## First boot

- WiFi: nothing is configured yet, so `pi-bluetooth-configuration` opens
  its fallback AP (SSID = the Pi's hardware serial) -- follow the normal
  setup flow in the iOS app from there. See the main
  [README](../README.md).
- SSH: `ssh root@<hostname>.local` (or its DHCP-assigned IP), password
  is whatever you set as `ROOT_PASSWORD`. Host keys are fresh every
  boot -- see "Config persistence across reboots" above.
- Relays: `pi-relay-control` starts with its default GPIO/port mapping
  from [`pi-relay-control.conf`](../../pi-relay-control-alpine/pi-relay-control.conf)
  baked into its own `.apk`; edit `/etc/pi-relay-control.conf` and
  `rc-service pi-relay-control restart` on the device to change it.
- Victron: `victron-ve-direct` starts pointed at `/dev/ttyUSB0` (the
  usual device node for a genuine Victron VE.Direct-to-USB cable, FTDI
  chipset -- driver autoloads via `mdev`'s hotplug handling on plug-in).
  Announces itself over mDNS/DNS-SD via `avahi-daemon`, which this
  image also installs and enables.

## Security note

This daemon's fallback AP is deliberately open (no password) so a phone
can join it during setup -- see the main README's Security model
section. That means, for however long the Pi is in fallback-AP mode,
anyone in range can also reach its SSH port over that same open network.
Change `ROOT_PASSWORD` to something you're comfortable with before
building, and consider switching to key-based auth once you're on the
device -- `/etc/ssh/sshd_config` changes made directly on a running
device now *do* survive a clean reboot (see "Config persistence across
reboots" above), so this can be done live rather than only by editing
`build-image.sh`'s `aipicam-setup.start` template and rebuilding.

Also note `victron-ve-direct`'s `allow_set = true` default in its
`config.ini` lets anyone who can reach its status port (`:8562`) change
charger settings -- see that repo's README if you want to lock that
down.

## Known limitations

- Filesystem verified (`fsck.vfat`) and the full offline `apk add`
  resolution independently simulated and confirmed successful on this
  Mac; not yet test-booted on real Pi 3 hardware -- please report back
  if you hit anything on first boot.
- Only tested/intended for a genuine Pi 3 (or other aarch64-capable
  board using the same `bcm2710`/`bcm2837`-family SoC); a Pi 4/5 would
  need its own verification even though the same aarch64 `.apk`s would
  technically install.
- Cloudflare Tunnel support: `cloudflared` itself was verified directly
  (confirmed statically linked, confirmed it actually runs inside an
  aarch64 Alpine container), and the generated `apkovl` contents
  (binary, token file, `cloudflared` OpenRC service, runlevel wiring)
  were verified by inspecting a real build's output. The
  `CLOUDFLARE_API_TOKEN` auto-provisioning path's request/response
  handling was checked against Cloudflare's real API for both endpoints
  (confirmed they exist and return the documented error shape when
  called with a fake token) and its JSON parsing verified against
  Cloudflare's documented success-response shape, but not yet exercised
  against a real authenticated account -- and the tunnel itself, either
  way, hasn't been tested against real hardware yet.
