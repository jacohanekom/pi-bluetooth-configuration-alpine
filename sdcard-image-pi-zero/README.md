# sdcard-image-pi-zero

Builds a bootable SD card image for **Raspberry Pi Zero and Zero W**
(`armhf` -- Alpine's ARMv6-with-hardware-float arch, matching this
board's BCM2835 SoC) with [pi-bluetooth-configuration](..),
[pi-relay-control](https://github.com/jacohanekom/pi-relay-control-alpine),
and [victron-ve-direct](https://github.com/jacohanekom/victron-ve-direct-alpine)
pre-installed and enabled -- write it to a card, boot it, and all three
daemons are already running.

This is the exact same diskless-mode approach as
[`../sdcard-image-pi3`](../sdcard-image-pi3) (see that directory's
README for the full "why diskless, not disk-resident" background and
the config-persistence/clock-reliability mechanisms -- all of it
applies here unchanged, none of it is architecture-specific). This
README only covers what's actually **different** for the Zero.

## Why a separate image directory, not one script for both boards

`arch=` (`armhf` vs `aarch64`), the official Alpine release tarball,
the `--platform` Docker needs for a genuine (QEMU-emulated, on typical
build machines) target binary, and the `cloudflared` binary/checksum
all differ per board -- and unlike the disk-resident era this project
moved away from, there's no shared disk-resident base image build to
factor these into instead. Keeping each board's image self-contained in
its own directory (same convention as `sdcard-image-pi3`) means every
file in here can be copied and diffed directly against its pi3
counterpart to see exactly what changed, rather than threading
conditionals through one shared script.

## armhf, not armv7 -- why this matters here specifically

Alpine's `armhf` arch targets **ARMv6** with hardware floating point --
exactly the BCM2835 in a Zero/Zero W. Alpine's `armv7` arch, despite
sharing "ARM" in the name, targets **Cortex-A7 and later** (a Pi 2 and
up) and simply won't boot here. Confirmed directly against Alpine's own
release server and Docker's own multi-arch `alpine` image manifest
(which lists `linux/arm/v6` and `linux/arm/v7` as genuinely distinct
platforms), not assumed. This is also exactly the distinction that made
Wetty (Node.js/V8) a real risk on this specific board -- V8 hasn't
properly supported ARMv6 in years -- which is why this project replaced
it with [ttyd](https://github.com/tsl0922/ttyd) (a plain C binary via
libwebsockets) project-wide before this image was built. Nothing
shipped in this image is Node.js-based any more; see
`../sdcard-image-pi3/README.md`'s "Web terminal (ttyd)" section for the
full history.

## Hardware differences from a Pi 3

- **No onboard Ethernet at all** (not even a limited/USB-bridged one) --
  a Zero/Zero W has exactly one data-capable port (the micro-USB one
  labeled "USB", not "PWR"). First internet access is therefore
  necessarily over WiFi, via the same fallback-AP-then-real-network flow
  the main [README](../README.md) already describes -- nothing about
  that flow changes here, since a Pi 3 build already goes through WiFi
  either way; it's just no longer optional the way it technically was
  on a board with an Ethernet jack.
- **Single USB port, shared** -- a genuine Victron VE.Direct-to-USB
  cable (what `victron-ve-direct` expects at `/dev/ttyUSB0`) needs that
  same port, via a USB OTG adapter. If you also want a keyboard at the
  physical console (see "Logging in: the admin account, not root" in
  the pi3 README -- `ROOT_PASSWORD` is console-only, same here), you'll
  need a powered USB hub rather than relying on the Zero's single port
  for both at once.
- **Single-core, no hardware floating-point-heavy workload here** --
  none of the three daemons or the packages this image installs
  (`hostapd`, `dnsmasq`, `avahi`, `dbus`, `ttyd`, `cloudflared`) are
  CPU-intensive; this is the same class of lightweight C-daemon
  workload Alpine's own diskless images are routinely run on Zero-class
  hardware for. No performance-driven changes were needed anywhere in
  this image relative to the pi3 build.
- **No Bluetooth radio distinction that matters here** -- despite the
  parent project's name, WiFi setup in this daemon has been HTTP+AP+mDNS
  based (not BLE) since the pivot documented in the main README; a Zero
  W's WiFi/BT combo chip is used the same way a Pi 3's is.

## Prerequisites

Same as `sdcard-image-pi3`, plus: since no machine most people build on
is genuinely ARMv6, every `docker run --platform linux/arm/v6 ...` step
in `build-image.sh` runs under QEMU emulation (`docker/setup-qemu-action@v3`
in CI, or Docker Desktop's built-in `binfmt_misc` support locally on
macOS) rather than natively -- this is slower than the pi3 build's
genuinely-native aarch64 run on Apple Silicon, but produces identical,
correct results (verified directly, not assumed -- see "Known
limitations" below).

## 1. Fetch the three armhf `.apk` artifacts

```sh
mkdir -p artifacts

gh run list -R jacohanekom/pi-bluetooth-configuration-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-bluetooth-configuration-alpine -n pi-bluetooth-configuration-apk-armhf -D artifacts

gh run list -R jacohanekom/pi-relay-control-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/pi-relay-control-alpine -n pi-relay-control-apk-armhf -D artifacts

gh run list -R jacohanekom/victron-ve-direct-alpine -L 1 --json databaseId -q '.[0].databaseId'
gh run download <run-id> -R jacohanekom/victron-ve-direct-alpine -n victron-ve-direct-apk-armhf -D artifacts
```

or attach a tagged release's `.apk` files there directly. Either way
you should end up with:

```
artifacts/pi-bluetooth-configuration-armhf.apk
artifacts/pi-relay-control-armhf.apk
artifacts/victron-ve-direct-armhf.apk
```

## 2. Build the image

```sh
ROOT_PASSWORD='something-you-choose' ./build-image.sh
```

Optional: `PI_HOSTNAME=whatever` (defaults to `aipicam`). Output is
`aipicam-pi-zero-diskless.img` (~768MB). Same `ROOT_PASSWORD`
handling, web terminal (ttyd), admin-account/doas login model, and
optional Cloudflare Tunnel setup as `sdcard-image-pi3` -- see that
directory's README for "Web terminal (ttyd)", "Logging in: the admin
account, not root", and "Remote access via Cloudflare Tunnel" in full;
none of it differs here except which `cloudflared` binary gets fetched:

```sh
ROOT_PASSWORD='something-you-choose' \
CLOUDFLARE_API_TOKEN='<a scoped Cloudflare API token>' \
CLOUDFLARE_ACCOUNT_ID='<your Cloudflare account ID>' \
CLOUDFLARE_ZONE_ID='<the zone ID owning CLOUDFLARE_DOMAIN>' \
CLOUDFLARE_DOMAIN='devices.example.com' \
./build-image.sh
```

`build-image.sh` fetches the `cloudflared-linux-armhf` GitHub release
asset here (not `cloudflared-linux-arm64`, the pi3 build's asset, or
`cloudflared-linux-arm`, which targets ARMv7+ and won't run on this
SoC), verified against its own pinned checksum -- confirmed directly
against the real release's asset list, not assumed.

## 3. Write it to an SD card

**Double-check the device path before running `dd` -- writing to the
wrong disk destroys its contents with no warning and no undo.**

```sh
diskutil list                      # find your SD card, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=aipicam-pi-zero-diskless.img of=/dev/rdisk4 bs=4m status=progress
sync
diskutil eject /dev/disk4
```

(Use the `/dev/rdiskN` "raw" device, not `/dev/diskN`, for a much faster
write on macOS.)

## Building in CI instead of locally

[`sdcard-image-pi-zero.yml`](../.github/workflows/sdcard-image-pi-zero.yml)
runs the same script under QEMU on a standard `ubuntu-24.04` runner --
unlike `sdcard-image-pi3.yml`, there's no genuine ARMv6 GitHub-hosted
runner to build natively on, so this one always emulates (see
"Prerequisites" above). Same `workflow_dispatch`-only trigger and repo
secrets as `sdcard-image-pi3.yml`:

- `SDCARD_ROOT_PASSWORD`, `CROSS_REPO_GH_TOKEN`,
  `SDCARD_CLOUDFLARE_API_TOKEN`, `SDCARD_CLOUDFLARE_ACCOUNT_ID`,
  `SDCARD_CLOUDFLARE_ZONE_ID`, `SDCARD_CLOUDFLARE_DOMAIN` -- same
  meaning as `sdcard-image-pi3.yml`'s own (see that workflow/README),
  shared across both images rather than duplicated per board.

Then trigger it from the Actions tab, or:

```sh
gh workflow run sdcard-image-pi-zero.yml -R jacohanekom/pi-bluetooth-configuration-alpine
```

Grab the result from the run's Artifacts section
(`aipicam-pi-zero-diskless`, kept 14 days).

## First boot

Same as `sdcard-image-pi3` -- see that README's "First boot" section.
The only Zero-specific note: expect first boot to take noticeably
longer than a Pi 3's (single ARMv6 core vs. a quad-core aarch64 SoC),
particularly the initial `apk` package installation from the local
repo during the diskless boot process.

## Security note

Same as `sdcard-image-pi3` -- see that README's "Security note"
section; nothing about the security model differs by board.

## Known limitations

- A full real run of `build-image.sh` (using placeholder `.apk`
  artifacts standing in for the three real daemons, built via a
  throwaway `abuild` package under QEMU armhf emulation) was verified
  end-to-end on this Mac, both without and with `CLOUDFLARE_API_TOKEN`
  set: `fsck.fat` reports a clean filesystem, the correct
  `bcm2835-rpi-zero*.dtb`/`bootcode.bin` boot files are present, the
  resulting apkovl was extracted and inspected directly to confirm it
  contains `ttyd` (not `wetty`/`nodejs` anywhere) with correct runlevel
  wiring, and -- with Cloudflare enabled -- the embedded `cloudflared`
  binary's SHA-256 matched the pinned `CLOUDFLARED_SHA256` exactly, byte
  for byte. Not yet run with the real daemon artifacts or test-booted on
  real Pi Zero/Zero W hardware -- please report back if you hit
  anything on first boot.
- Everything inherited unchanged from `sdcard-image-pi3` (the
  `wpa_supplicant.conf`-clobber fix, the admin-account/doas flow, the
  `provision-cloudflare.sh` control flow) carries the exact same
  verification status documented in that directory's own "Known
  limitations" section -- none of it is board-specific, so nothing
  further was re-verified here beyond confirming the armhf build itself
  produces the same correct output.
- Only tested/intended for a genuine Pi Zero or Zero W (`bcm2835`
  SoC); the second-generation Pi Zero 2 W uses a different,
  aarch64-capable SoC and should use the `sdcard-image-pi3` build
  instead (its own official Alpine release tarball supports it, per
  Alpine's own release notes) -- not verified directly here, since it
  isn't a board this session had a specific request to support.
