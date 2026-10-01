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

- **Everything under `/etc`** -- WiFi credentials
  (`/etc/wpa_supplicant/wpa_supplicant.conf`), SSH host keys, user
  account doas permits, the Ethernet gateway state file, and more -- is
  committed back to the boot partition by `pi-bluetooth-configuration`
  itself, calling `lbu commit -d mmcblk0p1` (Alpine's own diskless
  config-persistence tool -- the same one `setup-alpine`'s interactive
  wizard would normally wire up for you). WiFi, Ethernet, relay control,
  and user accounts/SSH are independent features with no shared
  "finish" step anymore (see the main README's "Independent
  WiFi/Ethernet/relay configuration, no reboot needed"), so each of
  their own routes (`POST /connect`, `POST /forget`, `POST /ethernet`,
  `POST /relay-control`, `POST /accounts`, `POST /accounts/remove`,
  `POST /ssh`, `POST /user`) commits on its own right after making its
  change, rather than all being batched behind one single checkpoint the
  way an earlier reboot-triggered design did.
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
  (though logged), losing whatever it was supposed to persist.
  Reproduced exactly against real Alpine tooling (not just inferred
  from source) before fixing. `/etc/apk/protected_paths.d/lbu.list`
  (baked into the apkovl at build time) is what tells `lbu` which paths
  to track -- just `/etc` itself; anything this daemon needs to persist
  has to live *under* `/etc` specifically (not bare at the filesystem
  root) because `lbu commit` actually works via `apk audit --backup`,
  which reliably tracks new/changed files *within* a protected directory
  but -- confirmed directly with a real test, not assumed -- silently
  never picks up a bare top-level file no matter how it's listed in
  `lbu.list`.
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
- **The daemon's own log** (`/var/log/pi-bluetooth-configuration.log`)
  is NOT persisted -- it lives on tmpfs like the rest of `/var` and is
  wiped on every reboot. Use the web UI's live status, or `tail -f` it
  before rebooting, if you need to debug something that only shows up
  around a reboot.

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
this image's previous Tailscale-based remote-access feature), not a
problem specific to whichever remote-access mechanism happened to be in
use.

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
in over SSH at all; see "Logging in: user accounts, not root" below for
how that actually works.

## Logging in: user accounts, not root

Root can no longer log in over SSH at all (`PermitRootLogin no`,
unconditional -- see build-image.sh's own comment). `ROOT_PASSWORD`
still gets hashed into `/etc/shadow` every boot as before, but it's now
only ever usable at a physical keyboard/monitor plugged directly into
the Pi.

The web UI itself now requires a login too (HTTP Basic Auth, checked
against this same `/etc/shadow` -- see the main README's Security
model) -- `root`/`ROOT_PASSWORD` always works for this, immediately,
since it's set at build time, so there's no chicken-and-egg problem
reaching "Users" on a fresh device even though SSH itself has nothing to
log into yet. The very first time you log in as `root` this way, the
page replaces itself entirely with a forced "change your password"
form -- every other route is locked behind HTTP 403 until you submit
one, since `ROOT_PASSWORD` is a known default (often identical across
every device built from the same image) rather than something unique to
this one device. See the main README's "Forced password change for
root".

There is no auto-generated SSH account of any kind -- a fresh device
has **no working SSH login at all** until you deliberately create one
from the web UI's "Users" section:

- **Add a user**: pick a username and password yourself (both are
  plain form fields -- there's no credential-reveal step, since you
  already know the password you just typed) and `POST /accounts`
  creates that Unix account, permitted to `doas` (Alpine's
  sudo-equivalent) to root. Log in with it (`ssh
  <user>@<hostname>.local`), then `doas <command>` (or `doas -s` for a
  root shell) whenever you actually need root.
- **Remove a user**: `POST /accounts/remove` deletes an account this
  daemon created. Refuses anything it didn't create itself (root,
  system accounts, etc.) -- see `src/main.cpp`'s `remove_account()`.
- **Enable/disable SSH itself**: the same section's SSH toggle
  (`POST /ssh`) starts/stops `sshd` live and adds/removes it from the
  default runlevel in the same call, so the choice survives a reboot
  too.

All of this is entirely independent of WiFi/Ethernet/relay state --
reachable the moment the device boots, before WiFi is ever configured,
over `eth0`/`eth1`. There's no recovery path if you lose every
account's password short of a physical console login (`ROOT_PASSWORD`)
to fix it by hand, or re-imaging the card.

Reachable only while you're on the same LAN (or plugged into
`eth0`/`eth1`) -- there is no remote-access/tunneling feature in this
image. If you need to reach a device remotely, set up your own solution
(e.g. a VPN) independently of this build.

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

Then trigger it from the Actions tab, or:

```sh
gh workflow run sdcard-image-pi3.yml -R jacohanekom/pi-bluetooth-configuration-alpine
```

Grab the result from the run's Artifacts section
(`aipicam-pi3-diskless`, kept 14 days).

## First boot

- WiFi: nothing is configured yet -- `eth0` is already up as a working
  gateway (default `192.168.4.1:8080`), so plug a laptop into it and
  open the web UI to scan for and join a network. See the main
  [README](../README.md).
- SSH: root login is disabled entirely, and no user account exists until
  you add one from the web UI's "Users" section (see
  "Logging in: user accounts, not root" above) -- entirely independent
  of whether WiFi has been configured yet. Once added,
  `ssh <user>@<hostname>.local` with the password you chose. Host keys
  are fresh every boot -- see "Config persistence across reboots" above.
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

This daemon's web UI and HTTP API are plain, unauthenticated HTTP,
reachable to anything wired into `eth0`/`eth1` (or already on whatever
WiFi network the Pi joined) -- see the main README's Security model
section. That doesn't buy anything SSH access on its own, though: root
login is disabled outright, and no user account exists until someone
deliberately adds one from the web UI's "Users" section --
so there's genuinely nothing to log into over SSH until that point,
regardless of who else can reach the web UI itself. `ROOT_PASSWORD`
still matters for physical console access (see "Logging in: user
accounts, not root" above), so change it to something you're
comfortable with before building regardless. Consider switching an
account to key-based auth once it exists -- `/etc/ssh/sshd_config`
changes made directly on a running device now *do* survive a clean
reboot (see "Config persistence across reboots" above), so this can be
done live.

Also note `victron-ve-direct`'s `allow_set = true` default in its
`config.ini` lets anyone who can reach its status port (`:8562`) change
charger settings -- see that repo's README if you want to lock that
down.

## Known limitations

- A full real run of `build-image.sh` (using placeholder `.apk`
  artifacts standing in for the three real daemons) was verified
  end-to-end on this Mac: `fsck.fat` reports a clean filesystem, and the
  resulting apkovl was extracted and inspected directly to confirm it
  actually contains what each feature is supposed to ship (correct
  runlevel wiring). Not yet run with the real daemon artifacts or
  test-booted on real Pi 3 hardware -- please report back if you hit
  anything on first boot.
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
- User accounts/doas: `adduser -D` + `openssl passwd -6` +
  `usermod -p <hash>` (the exact sequence `add_account()` runs) was
  verified end-to-end in a real Alpine container -- a genuine `ssh`
  login with a freshly-created account's password succeeded, and a
  `permit persist <user>` rule in `/etc/doas.d/*.conf` (root-owned,
  mode 0600) was confirmed to actually grant root via `doas`, including
  doas's own file-ownership/writability checks (refuses a config it
  doesn't own or that's group/other-writable, confirmed directly).
  `main.cpp`'s changes compile cleanly (`-Wall -Wextra`, no warnings) in
  this project's standard Docker verification, but the actual
  `POST /accounts`/`POST /accounts/remove`/`POST /ssh` HTTP flows
  (server compiled, no daemon-level integration test) haven't been
  exercised end-to-end on real hardware yet.
