#!/bin/bash
# Builds a bootable Raspberry Pi Zero / Zero W (armhf) SD card image with
# pi-bluetooth-configuration and pi-relay-control pre-installed and
# enabled, from the two repos' own CI-built .apk artifacts. See README.md
# in this directory for prerequisites, usage, and how to write the
# result to an SD card.
set -euo pipefail
cd "$(dirname "$0")"

ALPINE_VERSION=3.20
PI_HOSTNAME="${PI_HOSTNAME:-aipicam}"
BOOT_SIZE_MB=256
IMG_SIZE_MB=900
OUT_IMG="pi-bluetooth-configuration-sdcard-armhf.img"

BT_APK="artifacts/pi-bluetooth-configuration-armhf.apk"
RELAY_APK="artifacts/pi-relay-control-armhf.apk"
[ -f "$BT_APK" ] || { echo "missing $BT_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$RELAY_APK" ] || { echo "missing $RELAY_APK -- see README.md for how to fetch it" >&2; exit 1; }

: "${ROOT_PASSWORD:?set ROOT_PASSWORD in the environment (used once, at build time, to hash into /etc/shadow -- never stored in plaintext or committed)}"

rm -rf work
mkdir -p work/bootfs

echo "==> Hashing root password"
docker run --rm alpine:"$ALPINE_VERSION" sh -c \
	'apk add --no-cache openssl >/dev/null 2>&1; openssl passwd -6 "$1"' _ "$ROOT_PASSWORD" \
	> work/root.hash

echo "==> Building rootfs (armhf, under QEMU emulation)"
docker rm -f sdimg-rootfs-build >/dev/null 2>&1 || true
docker run --platform linux/arm/v6 --name sdimg-rootfs-build \
	-v "$PWD/artifacts:/work/apks:ro" \
	-v "$PWD/work/bootfs:/work/bootfs" \
	-v "$PWD/rootfs-setup.sh:/rootfs-setup.sh:ro" \
	alpine:"$ALPINE_VERSION" sh /rootfs-setup.sh "$PI_HOSTNAME" "$(cat work/root.hash)"
docker export sdimg-rootfs-build -o work/rootfs.tar
docker rm -f sdimg-rootfs-build >/dev/null 2>&1

echo "==> Assembling partition images and final .img (plain Alpine x86_64 container -- no loop devices/privileged mode needed)"
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
