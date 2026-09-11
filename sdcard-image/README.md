# sdcard-image

Builds a bootable SD card image for the original **Raspberry Pi Zero /
Zero W** (armhf) with [pi-bluetooth-configuration](..) and
[pi-relay-control](https://github.com/jacohanekom/pi-relay-control-alpine)
pre-installed and enabled -- write it to a card, boot it, and both
daemons are already running. Everything else (base Alpine system, RPi
kernel/firmware, WiFi/AP/DHCP stack) is assembled from scratch via
`apk`, the same way `alpine/APKBUILD` in each repo does, not copied from
a pre-existing image.

This produces a normal disk-resident ("sys"-style) install -- the same
shape as the actual deployed units this project targets -- not Alpine's
default diskless/apkovl boot mode.

## Prerequisites

- Docker Desktop (used for both the armhf QEMU emulation and the plain
  x86_64 filesystem-assembly step -- no Linux host, loop devices, or
  privileged containers needed; everything operates on plain image
  files via `mkfs.ext4 -d` and `mtools`, so this also runs unmodified on
  macOS).
- `gh` (GitHub CLI), authenticated, to fetch the two `.apk` build
  artifacts.
- ~1GB free disk space.

## 1. Fetch the two armhf `.apk` artifacts

Either grab them from each repo's latest successful CI run:

```sh
mkdir -p artifacts
gh run list -R jacohanekom/pi-bluetooth-configuration-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-bluetooth-configuration-alpine -n pi-bluetooth-configuration-apk-armhf -D artifacts

gh run list -R jacohanekom/pi-relay-control-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-relay-control-alpine -n pi-relay-control-apk-armhf -D artifacts
```

or attach a tagged release's `.apk` files there directly if you're
building from a specific version. Either way you should end up with:

```
artifacts/pi-bluetooth-configuration-armhf.apk
artifacts/pi-relay-control-armhf.apk
```

## 2. Build the image

```sh
ROOT_PASSWORD='something-you-choose' ./build-sd-image.sh
```

Optional: `PI_HOSTNAME=whatever` (defaults to `aipicam`). Output is
`pi-bluetooth-configuration-sdcard-armhf.img` (~900MB, fixed size --
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
sudo dd if=pi-bluetooth-configuration-sdcard-armhf.img of=/dev/rdisk4 bs=4m status=progress
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
- Storage: the image ships small (~900MB total: 256MB boot + ~640MB
  root) regardless of your card's real size, and a first-boot script
  automatically grows the root partition and filesystem to fill
  whatever's actually there -- same idea as Raspberry Pi OS's own
  first-boot resize. No manual `resize2fs` needed.
- Relays: `pi-relay-control` starts with its default GPIO/port mapping
  from [`pi-relay-control.conf`](../../pi-relay-control-alpine/pi-relay-control.conf)
  baked in at build time; edit `/etc/pi-relay-control.conf` and
  `rc-service pi-relay-control restart` on the device to change it.

## Security note

This daemon's fallback AP is deliberately open (no password) so a phone
can join it during setup -- see the main README's Security model
section. That means, for however long the Pi is in fallback-AP mode,
anyone in range can also reach its SSH port over that same open network.
Change `ROOT_PASSWORD` to something you're comfortable with before
building, and consider switching to key-based auth
(`PasswordAuthentication no` in `/etc/ssh/sshd_config`, plus your own
key in `/root/.ssh/authorized_keys`) once you're on the device.

## How it works

- `rootfs-setup.sh` runs inside a `--platform linux/arm/v6 alpine:3.20`
  container (QEMU-emulated, same technique the two repos' own CI uses
  for their `build-armhf`/`apk-armhf` jobs) and `apk add`s the base
  system, RPi kernel/firmware/WiFi packages, and the two `.apk`s built
  in step 1, then configures fstab, root's password, sshd, OpenRC
  runlevels, and a first-boot resize/SSH-keygen hook. The container's
  own filesystem becomes the root partition's contents via `docker
  export`.
- `build-sd-image.sh` extracts that export, builds the boot partition
  (FAT32, populated with `mtools` -- no mounting needed) and root
  partition (ext4, populated directly from a directory via `mkfs.ext4
  -d` -- also no mounting/loop devices needed) as separate flat files,
  then assembles them into one final `.img` with `parted` (operating
  directly on the image file) and `dd` at the correct byte offsets.
  Avoiding loop devices/privileged mode throughout is what makes this
  work unmodified on Docker Desktop for Mac.

## Known limitations

- Built and its filesystems verified (`fsck.vfat`, `e2fsck`) on this
  Mac; not yet test-booted on real Pi Zero / Zero W hardware -- please
  report back if you hit anything on first boot.
- Ethernet bridging (`eth0`+`eth1`) doesn't apply to the Pi Zero (no
  onboard Ethernet at all without a USB adapter) -- see the main
  README's "Bridging a second wired interface" section if you attach
  one.
