#!/bin/bash
# Builds a bootable Raspberry Pi 3 (aarch64) SD card image with
# pi-bluetooth-configuration, pi-relay-control, and victron-ve-direct
# pre-installed and enabled, from each repo's own CI-built .apk
# artifacts. See README.md in this directory for prerequisites, usage,
# and how to write the result to an SD card.
#
# Same technique as the sibling ../sdcard-image (Pi Zero/Zero W, armhf),
# adapted for a 64-bit-capable board: Docker on Apple Silicon runs
# --platform linux/arm64 natively (no QEMU), so this build is actually
# faster than the armhf one despite carrying an extra package.
set -euo pipefail
cd "$(dirname "$0")"

# 3.22, not 3.20/3.21 -- Alpine's linux-rpi kernel only gained the r8152
# driver (Realtek RTL8152/RTL8153 USB-Ethernet, e.g. the chip in
# Waveshare's ETH/USB HUB HAT (B)) starting with 3.22; verified absent
# in both 3.20 and 3.21's linux-rpi module tree, present in 3.22's. The
# three .apk artifacts are still the ones built by each repo's own CI
# against Alpine 3.20 -- musl and libstdc++ are both forward-compatible
# (a binary built on an older Alpine installs and runs fine on a newer
# one), verified by actually installing all three into a 3.22 rootfs
# here, not just assumed.
ALPINE_VERSION=3.22
PI_HOSTNAME="${PI_HOSTNAME:-aipicam}"
BOOT_SIZE_MB=256
IMG_SIZE_MB=1024
OUT_IMG="pi-bluetooth-configuration-sdcard-aarch64.img"

BT_APK="artifacts/pi-bluetooth-configuration-aarch64.apk"
RELAY_APK="artifacts/pi-relay-control-aarch64.apk"
VICTRON_APK="artifacts/victron-ve-direct-aarch64.apk"
[ -f "$BT_APK" ] || { echo "missing $BT_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$RELAY_APK" ] || { echo "missing $RELAY_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$VICTRON_APK" ] || { echo "missing $VICTRON_APK -- see README.md for how to fetch it" >&2; exit 1; }

: "${ROOT_PASSWORD:?set ROOT_PASSWORD in the environment (used once, at build time, to hash into /etc/shadow -- never stored in plaintext or committed)}"

rm -rf work
mkdir -p work/bootfs

echo "==> Hashing root password"
docker run --rm --platform linux/arm64 alpine:"$ALPINE_VERSION" sh -c \
	'apk add --no-cache openssl >/dev/null 2>&1; openssl passwd -6 "$1"' _ "$ROOT_PASSWORD" \
	> work/root.hash

echo "==> Building rootfs (aarch64, native -- no QEMU)"
docker rm -f sdimg-rootfs-build-pi3 >/dev/null 2>&1 || true
docker run --platform linux/arm64 --name sdimg-rootfs-build-pi3 \
	-v "$PWD/artifacts:/work/apks:ro" \
	-v "$PWD/work/bootfs:/work/bootfs" \
	-v "$PWD/rootfs-setup.sh:/rootfs-setup.sh:ro" \
	alpine:"$ALPINE_VERSION" sh /rootfs-setup.sh "$PI_HOSTNAME" "$(cat work/root.hash)"
docker export sdimg-rootfs-build-pi3 -o work/rootfs.tar
docker rm -f sdimg-rootfs-build-pi3 >/dev/null 2>&1

echo "==> Assembling partition images and final .img (plain Alpine x86_64/arm64 container -- no loop devices/privileged mode needed)"
# The rootfs tar (device nodes, setuid bits, etc.) is extracted *inside*
# this container's own writable layer, not on the macOS host -- a
# bind-mounted host directory can't faithfully represent those, and
# mkfs.ext4 -d's directory walk hits spurious "Permission denied" errors
# reading them back out through osxfs/virtiofs.
docker run --rm -v "$PWD/work:/work" -e BOOT_SIZE_MB="$BOOT_SIZE_MB" -e IMG_SIZE_MB="$IMG_SIZE_MB" -e PI_HOSTNAME="$PI_HOSTNAME" \
	alpine:"$ALPINE_VERSION" sh -c '
		set -e
		apk add --no-cache parted dosfstools mtools e2fsprogs >/dev/null

		mkdir -p /tmp/rootfs
		tar -C /tmp/rootfs -xf /work/rootfs.tar
		rm -f /work/rootfs.tar

		# Docker bind-mounts its own /etc/hostname, /etc/hosts,
		# /etc/resolv.conf into every running container, shadowing
		# whatever rootfs-setup.sh wrote there -- so those three are
		# written here instead, directly into the extracted rootfs.
		printf "%s\n" "$PI_HOSTNAME" > /tmp/rootfs/etc/hostname
		{
			printf "127.0.0.1\tlocalhost %s\n" "$PI_HOSTNAME"
			printf "::1\t\tlocalhost %s\n" "$PI_HOSTNAME"
			printf "127.0.1.1\t%s\n" "$PI_HOSTNAME"
		} > /tmp/rootfs/etc/hosts
		: > /tmp/rootfs/etc/resolv.conf

		truncate -s "${BOOT_SIZE_MB}M" /work/bootfs.img
		mkfs.vfat -F32 -n BOOT /work/bootfs.img >/dev/null
		mcopy -i /work/bootfs.img -s /work/bootfs/* ::

		truncate -s "$((IMG_SIZE_MB - BOOT_SIZE_MB - 1))M" /work/rootfs.img
		mkfs.ext4 -F -L rootfs -d /tmp/rootfs /work/rootfs.img >/dev/null

		truncate -s "${IMG_SIZE_MB}M" /work/final.img
		parted -s /work/final.img \
			mklabel msdos \
			mkpart primary fat32 1MiB "$((1 + BOOT_SIZE_MB))MiB" \
			set 1 boot on \
			mkpart primary ext4 "$((1 + BOOT_SIZE_MB))MiB" 100%

		dd if=/work/bootfs.img of=/work/final.img bs=1M seek=1 conv=notrunc status=none
		dd if=/work/rootfs.img of=/work/final.img bs=1M seek="$((1 + BOOT_SIZE_MB))" conv=notrunc status=none

		rm -f /work/bootfs.img /work/rootfs.img
	'

mv work/final.img "$OUT_IMG"
rm -rf work

echo "==> Done: $OUT_IMG ($(du -h "$OUT_IMG" | cut -f1))"
echo "    Write it to an SD card with (see README.md for the safety notes):"
echo "    sudo dd if=$OUT_IMG of=/dev/rdiskN bs=4m status=progress && sync"
