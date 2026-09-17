#!/bin/sh
# Runs inside a --platform linux/arm/v6 alpine:3.20 container. Builds up
# the container's own "/" into the root filesystem that will be copied
# onto the SD image's ext4 partition (via `docker export` after this
# script exits) -- see build-sd-image.sh.
set -eu

HOSTNAME="$1"
ROOT_HASH="$2"

apk update
apk upgrade --no-cache

# Base system + OpenRC (this daemon's own runtime deps, per
# pi-bluetooth-configuration-alpine/alpine/APKBUILD's depends= and
# pi-relay-control-alpine's liblgpio requirement) + the RPi kernel/
# firmware + openssh for headless access.
apk add --no-cache \
	alpine-base openrc \
	linux-rpi raspberrypi-bootloader \
	linux-firmware-brcm wireless-regdb \
	wpa_supplicant wpa_supplicant-openrc \
	dhcpcd dhcpcd-openrc \
	iproute2 dnsmasq dnsmasq-openrc iptables hostapd hostapd-openrc \
	libcrypto3 \
	e2fsprogs util-linux \
	openssh-server openssh-server-common \
	chrony chrony-openrc

# The two packages this image exists to carry -- built by each repo's
# own CI (arch="aarch64 armhf" in their alpine/APKBUILD) and copied into
# /work/apks by build-sd-image.sh before this script runs. Signed with a
# throwaway per-CI-run key, hence --allow-untrusted (same as the READMEs'
# own `apk add --allow-untrusted ./pi-*-armhf.apk` install instructions).
apk add --no-cache --allow-untrusted \
	/work/apks/pi-bluetooth-configuration-armhf.apk \
	/work/apks/pi-relay-control-armhf.apk

# /etc/hostname, /etc/hosts and /etc/resolv.conf are NOT touched here --
# Docker bind-mounts its own per-container versions over those exact
# paths for as long as this container is running, so writes here would
# just hit the bind mount and never reach the exported image. build-sd-
# image.sh writes them directly into the extracted rootfs on the host
# instead, after `docker export`.

# Root login: password-based SSH access, explicitly requested over a
# pubkey-only or no-SSH setup -- see the README's Security note about why
# this matters while the fallback AP is open. Written directly into
# /etc/shadow rather than `chpasswd` so no plaintext ever touches disk or
# shell history, even transiently.
awk -v h="$ROOT_HASH" 'BEGIN{FS=OFS=":"} $1=="root"{$2=h} {print}' /etc/shadow > /etc/shadow.new
mv /etc/shadow.new /etc/shadow
chmod 640 /etc/shadow

sed -i \
	-e 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' \
	-e 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' \
	/etc/ssh/sshd_config
# Regenerated on first boot instead (see openrc-local first-boot hook
# below) so every card doesn't ship with identical, publicly-known host
# keys baked in from this build.
rm -f /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub

# /boot lives on its own FAT32 partition (mmcblk0p1), built separately by
# build-sd-image.sh from these same files -- copy them out to the
# bind-mounted /work/bootfs first, then empty /boot in this ext4 image
# (it's just a mountpoint at runtime, per the fstab entry below).
# mkinitfs's default /etc/mkinitfs/mkinitfs.conf feature set (ext4 + mmc
# already included) is what produced /boot/initramfs-rpi as a side effect
# of installing linux-rpi above.
cp -a /boot/. /work/bootfs/
# /boot/boot -> "." is a self-referential compatibility symlink some
# tooling expects; config.txt itself references the flat top-level names
# (kernel=vmlinuz-rpi, not boot/vmlinuz-rpi), so it's not needed on the
# FAT partition -- and FAT32 has no symlinks anyway, so drop it rather
# than let mcopy (in build-sd-image.sh) choke on it.
rm -f /work/bootfs/boot
cat > /work/bootfs/cmdline.txt <<'EOF'
console=serial0,115200 console=tty1 root=/dev/mmcblk0p2 rootfstype=ext4 rootwait
EOF
find /boot -mindepth 1 -delete

cat > /etc/fstab <<'EOF'
/dev/mmcblk0p2  /      ext4  rw,relatime  0  1
/dev/mmcblk0p1  /boot  vfat  rw,relatime  0  2
EOF

rc-update add devfs sysinit
rc-update add dmesg sysinit
rc-update add mdev sysinit
# cmdline.txt has no explicit "rw", so the kernel mounts / read-only by
# default -- fsck/root/localmount are what check it, remount it
# read-write, and mount /boot (and anything else in fstab). Without
# these, root stays read-only for the whole boot: the first-boot
# script's ssh-keygen -A can't write /etc/ssh ("unable to initialize
# ssh key" -- a read-only filesystem, not a keygen problem), nor can
# anything else that writes to disk (logs, service state, the
# firstboot-done marker itself). Found by actually test-booting the
# aarch64/Pi 3 sibling image (sdcard-image-pi3) on real hardware --
# same missing services here since this is where that script was
# copied from. OpenRC resolves start order from each service's own
# depend() (fsck -> root -> localmount) regardless of the order they're
# added here.
rc-update add fsck boot
rc-update add root boot
rc-update add localmount boot
rc-update add hwclock boot
rc-update add modules boot
rc-update add sysctl boot
rc-update add hostname boot
rc-update add bootmisc boot
rc-update add syslog boot
rc-update add local default
rc-update add wpa_supplicant default
rc-update add dhcpcd default
rc-update add chronyd default
rc-update add sshd default
rc-update add pi-bluetooth-configuration default
rc-update add pi-relay-control default
rc-update add killprocs shutdown
rc-update add mount-ro shutdown
rc-update add savecache shutdown

# First-boot-only actions, run once via OpenRC's "local" service:
# 1. Grow the ext4 root partition (and filesystem) to fill whatever the
#    real SD card's actual size is -- the image file itself is built
#    much smaller than any real card, same idea as Raspberry Pi OS's own
#    first-boot resize.
# 2. Generate this device's own unique SSH host keys (removed above) so
#    every card flashed from this same image doesn't share host keys.
mkdir -p /etc/local.d
cat > /etc/local.d/firstboot.start <<'EOF'
#!/bin/sh
MARKER=/var/lib/firstboot-done
[ -f "$MARKER" ] && exit 0

ROOT_DEV=/dev/mmcblk0
ROOT_PART=2
sfdisk -f "$ROOT_DEV" -N "$ROOT_PART" <<SFDISK
,+
SFDISK
resize2fs "${ROOT_DEV}p${ROOT_PART}"

ssh-keygen -A

mkdir -p "$(dirname "$MARKER")"
touch "$MARKER"
EOF
chmod +x /etc/local.d/firstboot.start

cat > /etc/motd <<EOF
Welcome to $HOSTNAME (pi-bluetooth-configuration / pi-relay-control).
See https://github.com/jacohanekom/pi-bluetooth-configuration-alpine
EOF

rm -rf /var/cache/apk/*
