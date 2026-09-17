# sdcard-image-pi3

Builds a bootable SD card image for **Raspberry Pi 3 and other
64-bit-capable boards** (`aarch64`) with
[pi-bluetooth-configuration](..),
[pi-relay-control](https://github.com/jacohanekom/pi-relay-control-alpine),
and [victron-ve-direct](https://github.com/jacohanekom/victron-ve-direct-alpine)
pre-installed and enabled -- write it to a card, boot it, and all three
daemons are already running. Everything else (base Alpine system, RPi
kernel/firmware, WiFi/AP/DHCP stack, avahi/D-Bus for mDNS) is assembled
from scratch via `apk`, the same way `alpine/APKBUILD` in each repo
does, not copied from a pre-existing image.

This is the aarch64/Pi 3 sibling of [`../sdcard-image`](../sdcard-image)
(Pi Zero/Zero W, armhf, two packages) -- same technique, one more
package, and a genuinely native (not QEMU-emulated) build on Apple
Silicon since Pi 3 runs a 64-bit userland.

This produces a normal disk-resident ("sys"-style) install -- the same
shape as the actual deployed units this project targets -- not Alpine's
default diskless/apkovl boot mode.

## Building in CI instead of locally

[`sdcard-image-pi3.yml`](../.github/workflows/sdcard-image-pi3.yml) runs
the exact same two scripts on a genuinely aarch64 GitHub-hosted runner
(`ubuntu-24.04-arm`, no QEMU) instead of your own Mac. `workflow_dispatch`
only -- it isn't wired to `push` like the package repos' own `build.yml`s,
since it depends on all *three* repos' latest artifacts, not just this
one, and produces a ~1GB artifact you don't want piling up on every
commit.

One-time setup, two repo secrets on **pi-bluetooth-configuration-alpine**
(Settings -> Secrets and variables -> Actions):

- `SDCARD_ROOT_PASSWORD` -- same meaning as the local `ROOT_PASSWORD` env
  var below: hashed into `/etc/shadow` at build time, never stored in
  plaintext.
- `CROSS_REPO_GH_TOKEN` -- this repo's own `.apk` is fetched with the
  default `GITHUB_TOKEN` (scoped to this repo already), but pulling the
  other two repos' latest artifacts needs a token that can read *their*
  Actions runs too, which `GITHUB_TOKEN` can't do across repos. Mint a
  fine-grained PAT (https://github.com/settings/personal-access-tokens)
  scoped to just `pi-relay-control-alpine` and `victron-ve-direct-alpine`,
  with **Actions: Read-only** repository permission, and save it here.

Then trigger it from the Actions tab, or:

```sh
gh workflow run sdcard-image-pi3.yml -R jacohanekom/pi-bluetooth-configuration-alpine
```

Grab the result from the run's Artifacts section (`pi-bluetooth-configuration-sdcard-aarch64`,
kept 14 days).

## Building it locally instead

The rest of this README covers running it by hand on your own machine.

## Prerequisites

- Docker Desktop (used for both the aarch64 build -- genuinely native on
  Apple Silicon, no QEMU involved -- and the plain filesystem-assembly
  step; everything operates on plain image files via `mkfs.ext4 -d` and
  `mtools`, no loop devices or privileged containers needed, so this
  also runs unmodified on macOS).
- `gh` (GitHub CLI), authenticated, to fetch the three `.apk` build
  artifacts.
- ~1.2GB free disk space.

## 1. Fetch the three aarch64 `.apk` artifacts

Either grab them from each repo's latest successful CI run:

```sh
mkdir -p artifacts

gh run list -R jacohanekom/pi-bluetooth-configuration-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-bluetooth-configuration-alpine -n pi-bluetooth-configuration-apk-aarch64 -D artifacts

gh run list -R jacohanekom/pi-relay-control-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-relay-control-alpine -n pi-relay-control-apk-aarch64 -D artifacts

gh run list -R jacohanekom/victron-ve-direct-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/victron-ve-direct-alpine -n victron-ve-direct-apk-aarch64 -D artifacts
```

or attach a tagged release's `.apk` files there directly if you're
building from a specific version. Either way you should end up with:

```
artifacts/pi-bluetooth-configuration-aarch64.apk
artifacts/pi-relay-control-aarch64.apk
artifacts/victron-ve-direct-aarch64.apk
```

## 2. Build the image

```sh
ROOT_PASSWORD='something-you-choose' ./build-sd-image.sh
```

Optional: `PI_HOSTNAME=whatever` (defaults to `aipicam`). Output is
`pi-bluetooth-configuration-sdcard-aarch64.img` (~1GB, fixed size --
see "Storage" below for why it doesn't need to match your card's real
capacity).

`ROOT_PASSWORD` is only ever used in-memory to compute a SHA-512 crypt
hash (`openssl passwd -6`) that gets written into `/etc/shadow`; the
plaintext itself is never written to disk or committed anywhere.

## 3. Write it to an SD card

**Double-check the device path before running `dd` -- writing to the
wrong disk destroys its contents with no warning and no undo.**

```sh
diskutil list                      # find your SD card, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=pi-bluetooth-configuration-sdcard-aarch64.img of=/dev/rdisk4 bs=4m status=progress
sync
diskutil eject /dev/disk4
```

(Use the `/dev/rdiskN` "raw" device, not `/dev/diskN`, for a much faster
write on macOS.)

## First boot

- WiFi: nothing is configured yet, so `pi-bluetooth-configuration` opens
  its fallback AP (SSID = the Pi's hardware serial) -- follow the normal
  setup flow in the iOS app from there. See the main
  [README](../README.md).
- SSH: `ssh root@<hostname>.local` (or its DHCP-assigned IP), password
  is whatever you set as `ROOT_PASSWORD` above. Each card gets its own
  freshly generated SSH host keys on first boot (not baked into the
  image), so no two cards built from the same image share host keys.
- Storage: the image ships small (~1GB total: 256MB boot + ~760MB root)
  regardless of your card's real size, and a first-boot script
  automatically grows the root partition and filesystem to fill
  whatever's actually there -- same idea as Raspberry Pi OS's own
  first-boot resize. No manual `resize2fs` needed.
- Relays: `pi-relay-control` starts with its default GPIO/port mapping
  from [`pi-relay-control.conf`](../../pi-relay-control-alpine/pi-relay-control.conf)
  baked in at build time; edit `/etc/pi-relay-control.conf` and
  `rc-service pi-relay-control restart` on the device to change it.
- Victron: `victron-ve-direct` starts pointed at `/dev/ttyUSB0` (the
  usual device node for a genuine Victron VE.Direct-to-USB cable, FTDI
  chipset -- driver is built into the kernel image and autoloads on
  plug-in via `mdev`'s hotplug handling, no manual `modprobe` needed).
  Announces itself over mDNS/DNS-SD (`_victron-data._tcp` /
  `_victron-status._tcp`) via `avahi-daemon`, which this image also
  installs and enables. Edit `/etc/victron-ve-direct/config.ini` and
  `rc-service victron-ve-direct restart` to change the device path or
  ports.

## Security note

This daemon's fallback AP is deliberately open (no password) so a phone
can join it during setup -- see the main README's Security model
section. That means, for however long the Pi is in fallback-AP mode,
anyone in range can also reach its SSH port over that same open network.
Change `ROOT_PASSWORD` to something you're comfortable with before
building, and consider switching to key-based auth
(`PasswordAuthentication no` in `/etc/ssh/sshd_config`, plus your own
key in `/root/.ssh/authorized_keys`) once you're on the device.

Also note `victron-ve-direct`'s `allow_set = true` default in
`config.ini` lets anyone who can reach its status port (`:8562`) change
charger settings -- see that repo's README if you want to lock that
down before deploying somewhere less trusted than a home LAN.

## How it works

- `rootfs-setup.sh` runs inside a `--platform linux/arm64 alpine:3.22`
  container -- genuinely native aarch64 on Apple Silicon Docker Desktop,
  not QEMU-emulated like the sibling armhf image's build -- and `apk
  add`s the base system, RPi kernel/firmware/WiFi packages, avahi/D-Bus
  (for victron-ve-direct's mDNS announcement), and the three `.apk`s
  built in step 1, then configures fstab, root's password, sshd, OpenRC
  runlevels, and a first-boot resize/SSH-keygen hook. The container's
  own filesystem becomes the root partition's contents via `docker
  export`.
- Alpine version is pinned to 3.22, not the 3.20 the three `.apk`s were
  themselves built against -- 3.22 is the first release whose
  `linux-rpi` kernel includes the `r8152` driver (Realtek RTL8152/
  RTL8153 USB-Ethernet, e.g. Waveshare's ETH/USB HUB HAT (B)); verified
  absent from both 3.20 and 3.21's module tree. musl and libstdc++ are
  both forward-compatible, so the 3.20-built `.apk`s install and run
  fine on the newer 3.22 base -- verified by actually building this
  image with them, not just assumed.
- `build-sd-image.sh` extracts that export, builds the boot partition
  (FAT32, populated with `mtools` -- no mounting needed) and root
  partition (ext4, populated directly from a directory via `mkfs.ext4
  -d` -- also no mounting/loop devices needed) as separate flat files,
  then assembles them into one final `.img` with `parted` (operating
  directly on the image file) and `dd` at the correct byte offsets.
  Avoiding loop devices/privileged mode throughout is what makes this
  work unmodified on Docker Desktop for Mac.
- Raspberry Pi OS's own bootloader auto-selects the right device tree
  blob for the detected board revision (`bcm2710-rpi-3-b.dtb` etc. for a
  Pi 3) from whatever's in `/boot` -- the whole boot fileset is copied
  wholesale, same as the armhf image, no explicit `device_tree=` wiring
  needed. `raspberrypi-bootloader`'s aarch64 build ships `config.txt`
  with `arm_64bit=1` already set (verified against the actual package
  contents), unlike a from-scratch config which defaults to `0`.

## Known limitations

- Built and its filesystems verified (`fsck.vfat`, `e2fsck`) on this
  Mac; not yet test-booted on real Pi 3 hardware -- please report back
  if you hit anything on first boot.
- Only tested/intended for a genuine Pi 3 (or other aarch64-capable
  board using the same `bcm2710`/`bcm2837`-family SoC); a Pi 4/5 would
  need its own dtb/firmware verification even though the same aarch64
  `.apk`s would technically install.
- Ethernet bridging (`eth0`+`eth1`) isn't configured here -- see the
  main README's "Bridging a second wired interface" section if your Pi 3
  model has onboard Ethernet and you want to bridge a second interface.
