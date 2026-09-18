#!/bin/bash
# Builds a bootable Raspberry Pi 3 (aarch64) SD card image using Alpine's
# OWN official diskless release (kernel/initramfs/config.txt, genuinely
# tested by the Alpine project) plus a bundled, fully-offline-capable
# local apk repository and a pre-built apkovl overlay that installs and
# enables pi-bluetooth-configuration, pi-relay-control, and
# victron-ve-direct on every boot.
#
# This REPLACES an earlier disk-resident ("sys"-style) approach that
# hand-rolled fstab/inittab/fsck/localmount/hostname ourselves -- real
# test-booting on a Pi 3 surfaced multiple boot-sequence bugs that way
# (a wpa_supplicant/wlan0 startup race, root never remounted read-write
# since fsck/root/localmount were never enabled, hostname showing as
# "(none)"). Alpine's own diskless boot mode sidesteps that whole class
# of bug entirely -- root is tmpfs, so there's no fsck/root/localmount
# step to get wrong, and hostname/getty ordering is Alpine's own
# well-tested logic, not ours.
#
# See README.md for the full explanation of how this works and its
# trade-offs (state resets every boot unless explicitly persisted).
set -euo pipefail
cd "$(dirname "$0")"

ALPINE_VERSION=3.22
ALPINE_RELEASE=3.22.5
PI_HOSTNAME="${PI_HOSTNAME:-aipicam}"
IMG_SIZE_MB=768
OUT_IMG="aipicam-pi3-diskless.img"

BT_APK="artifacts/pi-bluetooth-configuration-aarch64.apk"
RELAY_APK="artifacts/pi-relay-control-aarch64.apk"
VICTRON_APK="artifacts/victron-ve-direct-aarch64.apk"
[ -f "$BT_APK" ] || { echo "missing $BT_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$RELAY_APK" ] || { echo "missing $RELAY_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$VICTRON_APK" ] || { echo "missing $VICTRON_APK -- see README.md for how to fetch it" >&2; exit 1; }

: "${ROOT_PASSWORD:?set ROOT_PASSWORD in the environment (used once, at build time, to hash into /etc/shadow -- never stored in plaintext or committed)}"

rm -rf work
mkdir -p work/bootfs work/repo/aarch64 work/apkovl

# ── 1. Fetch Alpine's own official diskless release ─────────────────────────
# This is the exact same kernel/initramfs/config.txt/dtbs/overlays real
# Raspberry Pi OS users get from alpinelinux.org -- genuinely tested by
# the Alpine project, not reconstructed by us. It already bundles a
# partial local apk repo (work/bootfs/apks/aarch64) covering the base
# system + a handful of common daemons; we extend that same repo below
# rather than replacing it.
echo "==> Fetching official alpine-rpi-$ALPINE_RELEASE-aarch64.tar.gz"
OFFICIAL_TARBALL="work/alpine-rpi-$ALPINE_RELEASE-aarch64.tar.gz"
if [ ! -f "$OFFICIAL_TARBALL" ]; then
	curl -fsSL -o "$OFFICIAL_TARBALL" \
		"https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/aarch64/alpine-rpi-${ALPINE_RELEASE}-aarch64.tar.gz"
fi
tar -C work/bootfs -xzf "$OFFICIAL_TARBALL"

# ── 2. Build an extended, fully-offline-capable local apk repository ───────
# Merges the official bundle's own packages with everything our three
# daemons additionally need (hostapd, dnsmasq, iptables, avahi, dbus,
# WiFi firmware) plus the three daemons' own .apk files, into one
# repository signed with a throwaway key generated fresh for this build.
# nlplug-findfs (the initramfs' own boot-media scanner) finds this via
# the .boot_repository marker file -- see README.md's "How it works".
echo "==> Fetching additional packages (offline dependency closure)"
cp work/bootfs/apks/aarch64/*.apk work/repo/aarch64/
docker run --rm --platform linux/arm64 -v "$PWD/work/repo/aarch64":/out alpine:"$ALPINE_VERSION" sh -c '
	set -e
	apk update -q
	apk fetch -R -o /out \
		hostapd hostapd-openrc \
		dnsmasq dnsmasq-openrc \
		iptables iptables-openrc \
		iproute2 \
		avahi avahi-openrc \
		dbus dbus-openrc \
		linux-firmware-brcm wireless-regdb \
		libgcc libstdc++
'
cp "$BT_APK" "$RELAY_APK" "$VICTRON_APK" work/repo/aarch64/

# apk resolves a repository package to <name>-<version>.apk based on the
# index's own P:/V: fields -- not whatever the file happened to be named
# on disk. The three CI artifacts are named "<name>-aarch64.apk" (an
# artifact-download convenience name), which apk can't match against
# the index it's about to build, and fails with "package mentioned in
# index not found" for exactly the same reason the noarch/rewrite-arch
# issue below does. Rename them to the real convention before indexing.
for pkg in pi-bluetooth-configuration pi-relay-control victron-ve-direct; do
	ver=$(docker run --rm -v "$PWD/work/repo/aarch64":/repo:ro alpine:"$ALPINE_VERSION" \
		sh -c "tar -xzOf /repo/$pkg-aarch64.apk .PKGINFO 2>/dev/null | awk -F' = ' '/^pkgver/{print \$2}'")
	mv "work/repo/aarch64/$pkg-aarch64.apk" "work/repo/aarch64/$pkg-$ver.apk"
done

echo "==> Building and signing the local repository index"
# --rewrite-arch aarch64 is load-bearing, not cosmetic: several packages
# above (the *-openrc subpackages, wireless-regdb, linux-firmware-*) are
# genuinely architecture-independent and declare "noarch" in their own
# .PKGINFO. The real Alpine build infrastructure rewrites this to the
# repository's actual arch when indexing (confirmed by inspecting the
# official bundle's own APKINDEX); apk's own fetch resolution for a
# package recorded as "noarch" in a --repository-fetched index expects
# it in a sibling noarch/ directory that doesn't exist here, and fails
# every such package with "package mentioned in index not found" if the
# index isn't rewritten the same way -- verified this exact failure and
# fix directly, not assumed.
docker run --rm --platform linux/arm64 -v "$PWD/work/repo":/repo alpine:"$ALPINE_VERSION" sh -c '
	set -e
	apk add --no-cache alpine-sdk >/dev/null
	abuild-keygen -a -n -q
	KEY=$(ls ~/.abuild/*.rsa | head -1)
	cd /repo/aarch64
	apk index --allow-untrusted --rewrite-arch aarch64 -o APKINDEX.unsigned.tar.gz *.apk
	abuild-sign -k "$KEY" APKINDEX.unsigned.tar.gz
	mv APKINDEX.unsigned.tar.gz APKINDEX.tar.gz
	cp "${KEY}.pub" "/repo/$(basename "${KEY}.pub")"
'
REPO_KEY=$(basename work/repo/*.rsa.pub)

# ── 3. Build the apkovl overlay ─────────────────────────────────────────────
# This is what actually gets applied on top of the base system every
# single boot (diskless mode rebuilds everything from the local repo
# fresh each time -- nothing here is a "first boot only" concern the
# way it would be on a disk-resident install).
echo "==> Building the apkovl overlay"
OVL=work/apkovl
mkdir -p "$OVL"/etc/apk/keys "$OVL"/etc/runlevels/boot "$OVL"/etc/runlevels/default \
	"$OVL"/etc/init.d "$OVL"/etc/local.d "$OVL"/var/lib

cp "work/repo/$REPO_KEY" "$OVL/etc/apk/keys/"

cat > "$OVL/etc/apk/world" <<-EOF
	alpine-base
	pi-bluetooth-configuration
	pi-relay-control
	victron-ve-direct
	hostapd
	dnsmasq
	iptables
	avahi
	dbus
	wpa_supplicant
	dhcpcd
	chrony
	openssh-server
	linux-firmware-brcm
	wireless-regdb
EOF

# Matches the SD card's actual device name once mounted at runtime (the
# same mountpoint nlplug-findfs used to find .boot_repository during
# boot persists across switch_root -- see README.md).
echo "/media/mmcblk0p1/apks" > "$OVL/etc/apk/repositories"

# Triggers the initramfs' own default-services logic (devfs/dmesg/mdev/
# hwdrivers/modloop/modules/sysctl/hostname/bootmisc/syslog/mount-ro/
# killprocs/savecache/firstboot/hwclock-or-swclock) -- Alpine's own
# well-tested core service set, not something we re-derive ourselves.
touch "$OVL/etc/.default_boot_services"

echo "$PI_HOSTNAME" > "$OVL/etc/hostname"
cat > "$OVL/etc/hosts" <<-EOF
	127.0.0.1	localhost $PI_HOSTNAME
	::1		localhost $PI_HOSTNAME
	127.0.1.1	$PI_HOSTNAME
EOF
cat > "$OVL/etc/motd" <<-EOF
	Welcome to $PI_HOSTNAME (pi-bluetooth-configuration / pi-relay-control / victron-ve-direct).
	See https://github.com/jacohanekom/pi-bluetooth-configuration-alpine
EOF

# Same wpa_supplicant/wlan0 startup race fix as the disk-resident image
# (see git history) -- the onboard SDIO WiFi driver measurably lags
# behind the rest of boot, and wpa_supplicant's own OpenRC start_pre()
# only checks for a wireless interface once, with no retry. Placed in
# the "boot" runlevel so it always completes before "default" (where
# wpa_supplicant lives), same reasoning as before.
cp wait-for-wlan.initd "$OVL/etc/init.d/wait-for-wlan"
chmod +x "$OVL/etc/init.d/wait-for-wlan"
ln -sf /etc/init.d/wait-for-wlan "$OVL/etc/runlevels/boot/wait-for-wlan"

ln -sf /etc/init.d/local "$OVL/etc/runlevels/default/local"
for svc in wpa_supplicant dhcpcd chronyd sshd dbus avahi-daemon \
	pi-bluetooth-configuration pi-relay-control victron-ve-direct; do
	ln -sf "/etc/init.d/$svc" "$OVL/etc/runlevels/default/$svc"
done

# alpine-baselayout's default /etc/shadow and openssh's default
# sshd_config don't exist yet when this overlay is unpacked (that
# happens BEFORE the packages that ship them are installed -- see
# README.md) so they can't just be static files here. This runs every
# boot via OpenRC's "local" service instead, once those packages are
# actually in place.
#
# "local" itself declares `depend() { after * }` -- it deliberately
# runs after every other default-runlevel service, INCLUDING sshd.
# sshd has therefore already started (with the stock, unmodified
# sshd_config -- PermitRootLogin prohibit-password) by the time this
# script edits the file on disk; a running sshd doesn't notice config
# changes without being told to, so root+password logins were being
# silently rejected despite the file ending up correct. `rc-service
# sshd restart` (not just editing the file) is what actually makes the
# new PermitRootLogin/PasswordAuthentication settings take effect.
ROOT_HASH=$(docker run --rm --platform linux/arm64 alpine:"$ALPINE_VERSION" sh -c \
	'apk add --no-cache openssl >/dev/null 2>&1; openssl passwd -6 "$1"' _ "$ROOT_PASSWORD")
cat > "$OVL/etc/local.d/aipicam-setup.start" <<EOF
#!/bin/sh
awk -v h='$ROOT_HASH' 'BEGIN{FS=OFS=":"} \$1=="root"{\$2=h} {print}' /etc/shadow > /etc/shadow.new
mv /etc/shadow.new /etc/shadow
chmod 640 /etc/shadow

sed -i \\
	-e 's/^#\\?PermitRootLogin.*/PermitRootLogin yes/' \\
	-e 's/^#\\?PasswordAuthentication.*/PasswordAuthentication yes/' \\
	/etc/ssh/sshd_config

ssh-keygen -A
rc-service sshd restart
EOF
chmod +x "$OVL/etc/local.d/aipicam-setup.start"

# Diskless mode resets /var to tmpfs every boot; symlink relay state
# onto the persistent boot media instead so "resume last position after
# reboot" (pi-relay-control's whole point) actually survives a reboot.
# The target directory is pre-created on the boot partition below.
ln -sf /media/mmcblk0p1/relay-state "$OVL/var/lib/relay_control"

( cd "$OVL" && tar czf "../../work/$PI_HOSTNAME.apkovl.tar.gz" --numeric-owner etc var )

# ── 4. Assemble the boot media contents ─────────────────────────────────────
echo "==> Assembling boot media contents"
rm -rf work/bootfs/apks/aarch64
cp -r work/repo/aarch64 work/bootfs/apks/aarch64
cp "work/repo/$REPO_KEY" work/bootfs/apks/
touch work/bootfs/apks/.boot_repository
cp "work/$PI_HOSTNAME.apkovl.tar.gz" work/bootfs/
mkdir -p work/bootfs/relay-state

# ── 5. Build the final single-partition .img ────────────────────────────────
# Diskless mode needs only one FAT32 partition (kernel, apks/, apkovl --
# everything) unlike the disk-resident image's separate boot+ext4
# layout. No loop devices/privileged mode needed -- same technique as
# the sibling disk-resident builds.
echo "==> Building the final .img"
docker run --rm --platform linux/arm64 -v "$PWD/work":/work alpine:"$ALPINE_VERSION" sh -c '
	set -e
	apk add --no-cache parted dosfstools mtools >/dev/null

	truncate -s "'"$IMG_SIZE_MB"'M" /work/bootfs.img
	mkfs.vfat -F32 -n AIPICAM /work/bootfs.img >/dev/null
	mcopy -i /work/bootfs.img -s /work/bootfs/* ::

	truncate -s "$(('"$IMG_SIZE_MB"' + 1))M" /work/final.img
	parted -s /work/final.img \
		mklabel msdos \
		mkpart primary fat32 1MiB 100% \
		set 1 boot on

	dd if=/work/bootfs.img of=/work/final.img bs=1M seek=1 conv=notrunc status=none
	rm -f /work/bootfs.img
'

mv work/final.img "$OUT_IMG"
rm -rf work

echo "==> Done: $OUT_IMG ($(du -h "$OUT_IMG" | cut -f1))"
echo "    Write it to an SD card with (see README.md for the safety notes):"
echo "    sudo dd if=$OUT_IMG of=/dev/rdiskN bs=4m status=progress && sync"
