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
- **WiFi credentials specifically also need a second, unrelated fix** on
  top of the `-d` one above -- `lbu commit` succeeding is necessary but
  not sufficient for them. `pi-bluetooth-configuration`'s own package
  ships a bare, no-network default `wpa_supplicant.conf` (so a
  genuinely fresh device still has a working `ctrl_interface` for
  `wpa_cli` -- see that file's own header comment), installed at exactly
  the same path a real network gets staged/saved into. Diskless mode
  reinstalls *every* package fresh from scratch on *every* boot (root is
  tmpfs), and never carries over apk's own "already installed, don't
  clobber a locally-modified config" bookkeeping (`/lib/apk/db` isn't
  part of what `lbu` persists -- only `/etc` is) -- so that reinstall,
  which happens right after the apkovl (with whatever real network was
  last saved) is unpacked, unconditionally overwrites it back to the
  bare default, every single time. Confirmed on real hardware: the
  provisioning marker (never shipped by any package) survived a reboot
  while a saved network (shipped by this one) silently didn't -- then
  reproduced exactly in isolation (stage a real network -> simulate the
  package reinstall clobbering it -> confirm it's gone) before fixing.
  The fix: `wifi_control.hpp` now also mirrors a successful
  stage/connect to `wpa_supplicant.conf.saved`, a path no package ever
  touches, and `restore-wifi-config.initd` (a new "boot" runlevel
  service, same placement/reasoning as `wait-for-wlan` above) copies it
  back over the just-clobbered live file before `wpa_supplicant`/this
  daemon's own boot-time join attempt ever reads it. `forget()` clears
  `.saved` too, so a reset isn't silently undone by the next boot.
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

## System clock reliability

This board has no battery-backed real-time clock, so every cold boot
starts with whatever time the kernel happens to have (often long in the
past) until `chronyd` corrects it over NTP. Alpine's own default
`chrony.conf` uses the legacy `initstepslew` directive, which only
attempts its one-time step correction once, at `chronyd`'s own very
first startup moment -- if network/DNS isn't fully ready right then
(plausible this early in boot), chrony falls back to slow incremental
slewing for every correction afterward, which is hopelessly inadequate
for a clock that's off by months. Confirmed on real hardware: `chronyc
tracking`/`sources -v` showed it had already correctly determined the
real time with every configured NTP source fully reachable, yet `date`
stayed wrong indefinitely -- the correction was calculated but never
applied. `fix-chrony-makestep.initd` (boot runlevel, unconditional)
replaces this with the modern `makestep 1.0 3` directive, which applies
on each of the first three sync updates rather than a single early-boot
attempt. A wrong clock breaks anything that validates HTTPS certificate
dates -- this is what was actually behind a `x509: certificate has
expired or is not yet valid` failure seen on real hardware (against
this image's previous Tailscale-based remote-access feature, before it
was replaced by Cloudflare Tunnel -- the same clock bug would just as
easily break cloudflared's own connection to Cloudflare's edge, or any
other HTTPS client), not a problem specific to whichever remote-access
mechanism happened to be in use.

Same clobbering concern as `wpa_supplicant.conf` (see above): chrony's
own package ships `/etc/chrony/chrony.conf`, which diskless mode's
every-boot-fresh package reinstall would otherwise silently overwrite
any customization of back to Alpine's default. Unlike WiFi credentials
there's no per-boot dynamic content to preserve here, so this just
rewrites the file outright in the `boot` runlevel (same timing as
`wait-for-wlan`/`restore-wifi-config`) rather than needing a save/
restore pair.

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
is never written to disk or committed anywhere. It's only usable at a
physical keyboard/monitor plugged into the Pi, though -- root can't log
in over SSH or Wetty at all; see "Logging in: the admin account, not
root" below for how those actually work.

## Web terminal (Wetty)

Every image also bakes in [Wetty](https://github.com/butlerx/wetty), a
browser-based terminal -- unconditional, not gated behind any env var,
since it's useful over the LAN/fallback AP on its own (e.g. from a
phone with no SSH client) even before you decide whether to also set up
Cloudflare Tunnel below. Alpine doesn't package it (it's an npm
package with a native addon, `node-pty`, not a single static binary),
so `build-image.sh` runs `npm install` once at build time, for this
image's own aarch64/musl target specifically (confirmed the compiled
native binding is genuinely `ELF 64-bit LSB shared object, ARM
aarch64`, not copied from anywhere), and bundles the result directly.

Visit `http://<hostname>.local:3000` (or the Pi's IP) from any browser
on the same LAN to get a full terminal -- Wetty spawns a real `ssh -t
localhost` subprocess per connection (`--force-ssh`, see
`wetty.initd`'s own comment for why that flag specifically matters when
running as root against localhost), so it's gated by the exact same
sshd as connecting with a regular SSH client -- this doesn't add or
remove any authentication of its own, just another way to reach the
same sshd. **Not root**, though, and not `ROOT_PASSWORD` either -- see
"Logging in: the admin account, not root" below.

## Logging in: the admin account, not root

Root can no longer log in over SSH or Wetty at all (`PermitRootLogin
no`, unconditional, not just for Wetty specifically -- see
build-image.sh's own comment on why a Wetty-only restriction wouldn't
actually be a real security boundary: Wetty's `--ssh-user` is a
preference it applies to itself, not something a connecting client is
bound by, and a raw `Remote-User` HTTP header or `/ssh/<user>` URL path
can override it outright). `ROOT_PASSWORD` still gets hashed into
`/etc/shadow` every boot as before, but it's now only ever usable at a
physical keyboard/monitor plugged directly into the Pi.

Instead, the very first time `POST /finish` actually succeeds (i.e. the
first time the iOS app's setup wizard completes) `pi-bluetooth-
configuration` generates a random username and password, creates that
Unix account, permits it to `doas` (Alpine's sudo-equivalent) to root,
and returns both in that same `/finish` response for the app to display
to you. This is the **only** time either is ever retrievable -- only a
SHA-512 password hash is kept on disk from then on, the same way
`ROOT_PASSWORD` itself has always been handled here, so write it down
somewhere. Log in with it the same way you would have with root
(`ssh <user>@<hostname>.local`, the web terminal, or over the tunnel
once Cloudflare Tunnel below is set up), then `doas <command>` (or
`doas -s` for a root shell) whenever you actually need root.

Reconfiguring an already-provisioned device (WiFi changes, etc. --
anything that calls `/finish` again) leaves this account untouched;
the response is a plain `{"ok":true}` with no credentials in it, same
as before this feature existed. There's no recovery path if you lose
the password short of wiping `/etc/admin-user` and the account itself
and letting the next `/finish` regenerate both from scratch.

## Remote access via Cloudflare Tunnel (optional)

By default the Wetty terminal above (and SSH directly) are only
reachable while you're on the same LAN (or its own fallback AP). If you
also want to reach it from anywhere -- e.g. it's deployed somewhere
without you physically present -- `build-image.sh` can bake in
[Cloudflare
Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/),
entirely opt-in, routing straight to that same Wetty terminal. All four
of these must be set together:

```sh
ROOT_PASSWORD='something-you-choose' \
CLOUDFLARE_API_TOKEN='<a scoped Cloudflare API token>' \
CLOUDFLARE_ACCOUNT_ID='<your Cloudflare account ID>' \
CLOUDFLARE_ZONE_ID='<the zone ID owning CLOUDFLARE_DOMAIN>' \
CLOUDFLARE_DOMAIN='devices.example.com' \
./build-image.sh
```

Leave `CLOUDFLARE_API_TOKEN` unset (the default) and none of this
applies -- no packages fetched, no binary downloaded, no service
enabled, nothing changes about the image.

Like Tailscale (this image's remote-access mechanism until this
feature replaced it), `cloudflared` only ever makes outbound
connections to Cloudflare's edge, so there's **no server for you to run
or maintain** and it works behind any NAT/firewall with no port
forwarding anywhere. Unlike Tailscale, though, a Cloudflare Tunnel has
no concept of "join" -- a tunnel *is* a specific set of DNS hostnames,
so a single secret shared by every device would make them
indistinguishable replicas of the *same* tunnel rather than separately
addressable devices. Each device therefore needs **its own tunnel,
credentials, and DNS record**, created via the Cloudflare API the first
time it has internet access, rather than Tailscale's simpler
"present a shared key" model. Alpine also doesn't package `cloudflared`
at all (confirmed directly, not assumed), so `build-image.sh` fetches
its official prebuilt aarch64 binary straight from a [pinned GitHub
release](https://github.com/cloudflare/cloudflared/releases),
verified against a checksum recorded in the script, and ships a
hand-written OpenRC service for it (`cloudflared.initd`) rather than
relying on a package-provided one.

`pi-bluetooth-configuration` itself (see `src/main.cpp`,
`provision_cloudflare_async()`) invokes `provision-cloudflare.sh` with
this device's hardware serial the first time it has internet access --
same identity already used for its hostname and AP SSID. That script
owns every Cloudflare-specific detail: it looks for (and cleans up) any
stale tunnel of the same name from an earlier interrupted attempt, then
calls the Cloudflare API to create a fresh tunnel named after this
device's serial, writes its credentials and an ingress rule mapping
`<serial>.<CLOUDFLARE_DOMAIN>` to Wetty (`http://localhost:3000`, see
"Web terminal (Wetty)" above) under `/etc/cloudflared/`, upserts the
CNAME routing that hostname to the new tunnel, and only then enables
and starts the `cloudflared` service.
Retried roughly every 30s if any step fails (most likely: no internet
yet -- a freshly unconfigured device sits in its own fallback AP with
no uplink at all until WiFi setup finishes), idempotent (skips entirely
once `/etc/cloudflared/config.yml` already exists, which the script
only ever writes as its very last step). This is what makes the *same
built image* flashable onto any number of physical Pis -- each
provisions its own tunnel automatically, with no per-device step on
your end beyond flashing the card (the per-device *orchestration* is
real, unlike Tailscale, but it's the API token doing that work at
first boot, not you).

Once provisioned, reach it from any browser, anywhere -- no
`cloudflared` (or any other client software) needed on the connecting
machine at all, since the tunnel is terminated at Cloudflare's edge as
plain HTTPS:

```
https://<serial>.devices.example.com
```

**One-time setup**:

1. Find your **Account ID** on the right-hand sidebar of any zone's
   Overview page in the [Cloudflare
   dashboard](https://dash.cloudflare.com); that's
   `CLOUDFLARE_ACCOUNT_ID` above.
2. Find the **Zone ID** for the domain you want devices under
   (`devices.example.com` above can be a subdomain of a zone you
   already own, e.g. the zone is `example.com`) on that same Overview
   page; that's `CLOUDFLARE_ZONE_ID`.
3. [dash.cloudflare.com/profile/api-tokens](https://dash.cloudflare.com/profile/api-tokens)
   -> **Create Token** -> **Custom token** -> permissions
   **Account / Cloudflare Tunnel / Edit** and **Zone / DNS / Edit**,
   with **Account Resources** and **Zone Resources** both scoped to
   just this one account/zone (not "All accounts"/"All zones" -- this
   token can create and delete tunnels and DNS records within whatever
   it's scoped to, so keep that blast radius as small as possible).
   Copy the generated token; that's `CLOUDFLARE_API_TOKEN` above.

#### Security note

The API token lives on the device's filesystem
(`/etc/cloudflare-api-token`, mode 600) only *until* this device
successfully provisions its own tunnel -- `provision-cloudflare.sh`
deletes it once that succeeds, since a device with its own established
tunnel identity never needs to re-present it. This matters *more* here
than it did for Tailscale's reusable auth key: that key could only
register one more node onto an already-scoped tailnet, while this
token can create or delete **any** tunnel and DNS record within
whatever account/zone scope you gave it. A device compromised *before*
its first successful provisioning exposes that account/zone-wide
capability (bounded by the token's own scope, hence scoping it tightly
above); a device compromised *after* only exposes that one device's own
tunnel credentials and its one Wetty endpoint, not the ability to touch
your Cloudflare account further.

Note also that a Cloudflare Tunnel by itself only provides
*connectivity*, not *authentication* -- reaching the tunnel gets you to
Wetty, which (via `--force-ssh`) is itself just another way to reach
this Pi's own sshd, gated by the admin account's own password (root
login is disabled outright -- see "Logging in: the admin account, not
root" above) or SSH keys, same as LAN access. If you want an
identity-based access gate in front of it too (recommended for
anything reachable from the public internet), configure a [Cloudflare
Access application](https://developers.cloudflare.com/cloudflare-one/policies/access/)
for `*.CLOUDFLARE_DOMAIN` in the dashboard -- that's a manual,
account-wide policy decision independent of any single device, so it's
outside what this build/provisioning automation sets up for you.

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
- `SDCARD_CLOUDFLARE_API_TOKEN`, `SDCARD_CLOUDFLARE_ACCOUNT_ID`,
  `SDCARD_CLOUDFLARE_ZONE_ID`, `SDCARD_CLOUDFLARE_DOMAIN` (optional) --
  same meaning as the local `CLOUDFLARE_API_TOKEN`/`CLOUDFLARE_ACCOUNT_ID`/
  `CLOUDFLARE_ZONE_ID`/`CLOUDFLARE_DOMAIN` env vars above. Leave all
  four unset to skip Cloudflare Tunnel entirely, same as locally.
  Secrets rather than `workflow_dispatch` inputs since these are real
  credentials/identifiers, not a cosmetic setting like `hostname`.

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
- SSH/Wetty: neither works yet at this point -- root login is disabled
  entirely and the admin account doesn't exist until the app's wizard
  actually finishes (see "Logging in: the admin account, not root"
  above). Once it does, `ssh <user>@<hostname>.local` or
  `http://<hostname>.local:3000`, using the credentials the app showed
  you. Host keys are fresh every boot -- see "Config persistence across
  reboots" above.
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
section. Anyone in range can join that same open network for however
long the Pi is in fallback-AP mode, but that no longer buys them SSH or
Wetty access the way it used to before this account model existed:
root login is disabled outright, and the admin account this image now
relies on for everything doesn't exist until the app's own wizard
actually finishes -- so there's genuinely nothing to log into yet at
that point either way. `ROOT_PASSWORD` still matters for physical
console access (see "Logging in: the admin account, not root" above),
so change it to something you're comfortable with before building
regardless. Consider switching the admin account to key-based auth once
it exists -- `/etc/ssh/sshd_config` changes made directly on a running
device now *do* survive a clean reboot (see "Config persistence across
reboots" above), so this can be done live.

Also note `victron-ve-direct`'s `allow_set = true` default in its
`config.ini` lets anyone who can reach its status port (`:8562`) change
charger settings -- see that repo's README if you want to lock that
down.

## Known limitations

- A full real run of `build-image.sh` (using placeholder `.apk`
  artifacts standing in for the three real daemons) was verified
  end-to-end on this Mac: `fsck.fat` reports a clean filesystem, and the
  resulting apkovl was extracted and inspected directly to confirm it
  actually contains what each feature is supposed to ship (Wetty's
  compiled `node_modules`, the `cloudflared` binary matching its pinned
  checksum byte-for-byte, correct runlevel wiring). Not yet run with the
  real daemon artifacts or test-booted on real Pi 3 hardware -- please
  report back if you hit anything on first boot.
- The `wpa_supplicant.conf`-gets-clobbered-every-boot fix (see "Config
  persistence across reboots") was reproduced and verified in isolation
  (a real, deterministic simulation of the exact stage/clobber/restore
  sequence, not just inferred from reading the code) but hasn't yet
  been re-verified with an actual boot-clobber-restore cycle on real Pi
  3 hardware.
- Only tested/intended for a genuine Pi 3 (or other aarch64-capable
  board using the same `bcm2710`/`bcm2837`-family SoC); a Pi 4/5 would
  need its own verification even though the same aarch64 `.apk`s would
  technically install.
- Wetty: the exact pinned `WETTY_VERSION` installs cleanly for
  aarch64/musl and its native `node-pty` binding was confirmed to
  actually be a freshly-compiled `ELF 64-bit LSB shared object, ARM
  aarch64`, not a copied prebuilt for the wrong platform; a live
  instance was confirmed to serve real HTTP traffic. The root-on-
  localhost-spawns-`login`-instead-of-`ssh` behavior `--force-ssh`
  works around was confirmed by reading Wetty's own installed source
  directly, not assumed or inferred from its docs. Not yet exercised via
  an actual browser session logging in over websockets end-to-end, and
  not yet tested on real hardware.
- Admin account/doas: `adduser -D` + `openssl passwd -6` +
  `usermod -p <hash>` (the exact sequence `create_admin_account()`
  runs) was verified end-to-end in a real Alpine container -- a genuine
  `ssh` login with the freshly-generated password succeeded, and a
  `permit persist <user>` rule in `/etc/doas.d/*.conf` (root-owned,
  mode 0600) was confirmed to actually grant root via `doas`, including
  doas's own file-ownership/writability checks (refuses a config it
  doesn't own or that's group/other-writable, confirmed directly).
  `main.cpp`'s changes compile cleanly (`-Wall -Wextra`, no warnings) in
  this project's standard Docker verification, but the actual `/finish`
  HTTP flow (server compiled, no daemon-level integration test) and a
  real boot's very first setup-to-credentials round trip haven't been
  exercised end-to-end yet -- the iOS app also needs a corresponding
  update to read and display `adminUsername`/`adminPassword` from that
  response, which is outside this repo.
- Cloudflare Tunnel support: confirmed directly (not assumed) that
  Alpine packages neither `cloudflared` nor an OpenRC service for it,
  and confirmed the pinned `CLOUDFLARED_VERSION`/`CLOUDFLARED_SHA256`
  against the real GitHub release. `provision-cloudflare.sh`'s own
  control flow (tunnel create, stale-tunnel cleanup and retry, DNS
  record upsert, credentials/config.yml written correctly, API token
  deletion, and idempotent skip on a second run) was exercised
  end-to-end against a mock Cloudflare API in a real Alpine container,
  including the API-error and no-connectivity retry paths -- but it
  hasn't yet been run against a real Cloudflare account, and neither
  this feature nor real hardware has been tested on an actual boot yet.
