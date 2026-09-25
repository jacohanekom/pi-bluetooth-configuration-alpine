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
# Pinned the same way ALPINE_RELEASE above is -- reproducible builds,
# not whatever happens to be "latest" on the day this runs. SHA256 is
# GitHub's own per-asset digest (returned by the releases API, not
# computed by us) for cloudflared-linux-arm64 at this exact tag; bump
# both together when updating.
CLOUDFLARED_VERSION=2026.9.3
CLOUDFLARED_SHA256=aaeb2d7d0da3614634c7e03ab13487a1522c2e79165ed2929cfe23d5e95b326d
# Pinned for the same reason as CLOUDFLARED_VERSION -- npm's own
# resolution of a bare "wetty" would silently drift between builds
# otherwise. Verified this exact version installs and its node-pty
# native binding compiles cleanly for aarch64/musl inside the
# alpine:$ALPINE_VERSION container used below (confirmed the resulting
# .node file is genuinely `ELF 64-bit LSB shared object, ARM aarch64`,
# not a copied prebuilt for the wrong platform).
WETTY_VERSION=3.2.2
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

# Optional: Cloudflare Tunnel, for reaching this Pi's Wetty web terminal
# (see the unconditional Wetty setup further down) from outside its own
# LAN, with no self-hosted server and no inbound port forwarding
# anywhere -- cloudflared only ever makes outbound
# connections to Cloudflare's edge. Unlike a single reusable secret
# shared by every device (as Tailscale's own auth key was), a Tunnel has
# no concept of "join": each device needs its OWN tunnel, own
# credentials, and own DNS hostname, so this needs a scoped Cloudflare
# API token baked in instead -- provision-cloudflare.sh (invoked by
# pi-bluetooth-configuration itself once this device has internet
# access) uses it to create this device's tunnel and DNS record via the
# Cloudflare API, named after this device's hardware serial -- same
# reasoning as hostname/AP SSID already being that serial, not something
# shared across a whole image. All four of these must be set together;
# leave CLOUDFLARE_API_TOKEN unset (the default) and none of this gets
# added at all. See README.md, "Remote access via Cloudflare Tunnel".
CLOUDFLARE_API_TOKEN="${CLOUDFLARE_API_TOKEN:-}"
CLOUDFLARE_ACCOUNT_ID="${CLOUDFLARE_ACCOUNT_ID:-}"
CLOUDFLARE_ZONE_ID="${CLOUDFLARE_ZONE_ID:-}"
CLOUDFLARE_DOMAIN="${CLOUDFLARE_DOMAIN:-}"
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	: "${CLOUDFLARE_ACCOUNT_ID:?CLOUDFLARE_API_TOKEN is set -- CLOUDFLARE_ACCOUNT_ID must be too, see README.md}"
	: "${CLOUDFLARE_ZONE_ID:?CLOUDFLARE_API_TOKEN is set -- CLOUDFLARE_ZONE_ID must be too, see README.md}"
	: "${CLOUDFLARE_DOMAIN:?CLOUDFLARE_API_TOKEN is set -- CLOUDFLARE_DOMAIN must be too, see README.md}"
fi

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
CLOUDFLARE_PACKAGES=""
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	# curl + jq: what provision-cloudflare.sh needs on-device to call the
	# Cloudflare API and parse its JSON responses. cloudflared itself is
	# NOT an apk package (Alpine doesn't ship one) -- it's fetched
	# separately below, straight into the apkovl, not through this repo.
	CLOUDFLARE_PACKAGES="curl jq"
fi
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
		libgcc libstdc++ \
		nodejs openssh-client \
		doas shadow \
		'"$CLOUDFLARE_PACKAGES"'
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
mkdir -p "$OVL"/etc/apk/keys "$OVL"/etc/apk/protected_paths.d \
	"$OVL"/etc/runlevels/boot "$OVL"/etc/runlevels/default "$OVL"/etc/runlevels/shutdown \
	"$OVL"/etc/init.d "$OVL"/etc/local.d "$OVL"/etc/doas.d "$OVL"/var/lib \
	"$OVL"/usr/local/bin "$OVL"/usr/local/lib

cp "work/repo/$REPO_KEY" "$OVL/etc/apk/keys/"

cat > "$OVL/etc/apk/world" <<-EOF
	alpine-base
	alpine-conf
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
	nodejs
	openssh-client
	doas
	shadow
EOF
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	{
		echo "curl"
		echo "jq"
	} >> "$OVL/etc/apk/world"
fi

# What `lbu commit mmcblk0p1` (called by pi-bluetooth-configuration
# itself, right before it reboots at the end of a successful setup --
# see main.cpp's reboot_after_delay()) captures back into the apkovl:
# /etc wholesale -- SSH host keys, wpa_supplicant's saved credentials,
# sshd_config, shadow, our own runlevels/local.d/apk state, AND the
# provisioning marker pi-relay-control's own start_pre() refuses to
# start without (pi-bluetooth-configuration's MARKER_FILE lives at
# /etc/successfully-initialized specifically, not bare at "/", for
# exactly this reason -- see that repo's own comment on MARKER_FILE:
# `apk audit --backup`, what `lbu commit` uses under the hood,
# reliably tracks new/changed files *within* a protected directory but
# -- confirmed directly -- silently never picks up a bare top-level
# file no matter how it's listed here). Without this, none of it would
# survive a reboot -- diskless mode's root is tmpfs, rebuilt from
# scratch (this same apkovl) every single boot.
#
# An earlier version of this ran the commit from a generic OpenRC
# shutdown-runlevel service instead, relying on it always running
# before the actual reboot; that failed on real hardware for reasons
# not fully root-caused, so it's now triggered explicitly by the one
# process that actually knows a reboot is about to happen, instead.
cat > "$OVL/etc/apk/protected_paths.d/lbu.list" <<-EOF
	+etc
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

# Restores a WiFi network staged/saved on a previous boot before
# wpa_supplicant starts and this daemon attempts its own boot-time join
# -- see restore-wifi-config.initd and wifi_control.hpp's own comment
# on WPA_SUPPLICANT_CONF_SAVED for the full explanation of why this is
# needed at all (pi-bluetooth-configuration's own package ships a bare
# default wpa_supplicant.conf, which diskless mode's every-boot fresh
# package reinstall silently clobbers a real saved network with,
# confirmed on real hardware). Unconditional, not gated behind any
# opt-in flag -- this fixes core WiFi persistence, not an optional
# feature.
cp restore-wifi-config.initd "$OVL/etc/init.d/restore-wifi-config"
chmod +x "$OVL/etc/init.d/restore-wifi-config"
ln -sf /etc/init.d/restore-wifi-config "$OVL/etc/runlevels/boot/restore-wifi-config"

# Fixes chrony's default config so it actually steps (jumps) the clock
# on first sync instead of getting stuck slewing forever -- see
# fix-chrony-makestep.initd for the full explanation. Also unconditional
# -- a wrong clock breaks any HTTPS client that validates certificate
# dates, cloudflared's own connection to Cloudflare's edge very much
# included, not just this specific optional feature.
cp fix-chrony-makestep.initd "$OVL/etc/init.d/fix-chrony-makestep"
chmod +x "$OVL/etc/init.d/fix-chrony-makestep"
ln -sf /etc/init.d/fix-chrony-makestep "$OVL/etc/runlevels/boot/fix-chrony-makestep"

# Wetty, a browser-based terminal (xterm.js talking to a real `ssh`
# subprocess over a websocket) -- unconditional, unlike Cloudflare
# Tunnel: useful over the LAN/fallback AP on its own (e.g. from a phone
# with no SSH client), and becomes the Cloudflare Tunnel's ingress
# target when that feature is also enabled (see provision-cloudflare.sh).
# Not an apk package (nowhere in Alpine or otherwise) and not a single
# static binary either -- it's an npm package with a native addon
# (node-pty) that needs compiling, so build it once here, for this
# image's own target arch, and bundle the result directly (nodejs
# itself, to actually run it, comes from the local apk repo above like
# any other package).
echo "==> Building Wetty $WETTY_VERSION (npm install, aarch64-native)"
mkdir -p work/wetty
docker run --rm --platform linux/arm64 -v "$PWD/work/wetty":/wetty -w /wetty alpine:"$ALPINE_VERSION" sh -c '
	set -e
	apk add --no-cache nodejs npm python3 build-base >/dev/null
	npm init -y >/dev/null
	npm install wetty@'"$WETTY_VERSION"' --omit=dev --omit=optional >/dev/null
	# node-pty ships prebuilt native bindings for several platforms this
	# image will never run on; only the linux-arm64 one -- compiled
	# fresh just above, for this exact musl/aarch64 target, not copied
	# from anywhere -- is ever used here. Pruning the rest is a real
	# size saving (roughly 60MB), not just tidiness.
	find node_modules -type d \( -name "win32-*" -o -name "darwin-*" \) -exec rm -rf {} + 2>/dev/null || true
'
rm -rf "$OVL/usr/local/lib/wetty"
mkdir -p "$OVL/usr/local/lib/wetty"
cp -r work/wetty/node_modules "$OVL/usr/local/lib/wetty/"

cp wetty.initd "$OVL/etc/init.d/wetty"
chmod +x "$OVL/etc/init.d/wetty"

# Cloudflare Tunnel, for reaching this Pi remotely -- see the
# CLOUDFLARE_API_TOKEN check near the top of this script and README.md's
# "Remote access via Cloudflare Tunnel". Unlike tailscale-openrc's own
# package, Alpine ships nothing for cloudflared at all -- fetch the
# official prebuilt aarch64 binary directly from its GitHub release
# (verified against the pinned CLOUDFLARED_SHA256 above, not trusted
# blind) rather than inventing our own build of it.
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	echo "==> Fetching cloudflared $CLOUDFLARED_VERSION (aarch64)"
	mkdir -p "$OVL/usr/local/bin"
	CLOUDFLARED_BIN="$OVL/usr/local/bin/cloudflared"
	curl -fsSL -o "$CLOUDFLARED_BIN" \
		"https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-arm64"
	if command -v sha256sum >/dev/null 2>&1; then
		ACTUAL_SHA=$(sha256sum "$CLOUDFLARED_BIN" | cut -d' ' -f1)
	else
		ACTUAL_SHA=$(shasum -a 256 "$CLOUDFLARED_BIN" | cut -d' ' -f1)
	fi
	[ "$ACTUAL_SHA" = "$CLOUDFLARED_SHA256" ] || {
		echo "cloudflared download checksum mismatch: got $ACTUAL_SHA, expected $CLOUDFLARED_SHA256" >&2
		exit 1
	}
	chmod +x "$CLOUDFLARED_BIN"

	cp cloudflared.initd "$OVL/etc/init.d/cloudflared"
	chmod +x "$OVL/etc/init.d/cloudflared"

	cp provision-cloudflare.sh "$OVL/etc/cloudflare-provision.sh"
	chmod +x "$OVL/etc/cloudflare-provision.sh"

	# Kept in their own files (mode 600, read by
	# provision-cloudflare.sh), same treatment as the WiFi password and
	# root's own hashed password elsewhere in this build. The API token
	# is deleted by that script once this device has actually
	# provisioned its own tunnel, since it's only ever needed for a
	# device's initial provisioning (see that script's own comment) --
	# unlike Tailscale's node identity, this device's tunnel credentials
	# end up under /etc (config.yml + the credentials JSON, both
	# persisted by the same `+etc` lbu tracking as everything else here),
	# so no separate /var symlink onto the boot partition is needed.
	printf '%s' "$CLOUDFLARE_API_TOKEN" > "$OVL/etc/cloudflare-api-token"
	printf '%s' "$CLOUDFLARE_ACCOUNT_ID" > "$OVL/etc/cloudflare-account-id"
	printf '%s' "$CLOUDFLARE_ZONE_ID" > "$OVL/etc/cloudflare-zone-id"
	printf '%s' "$CLOUDFLARE_DOMAIN" > "$OVL/etc/cloudflare-domain"
	chmod 600 "$OVL/etc/cloudflare-api-token" "$OVL/etc/cloudflare-account-id" \
		"$OVL/etc/cloudflare-zone-id" "$OVL/etc/cloudflare-domain"

	# NOT added to any runlevel here, unlike tailscale-openrc's service --
	# there's no config for cloudflared to start against until
	# provision-cloudflare.sh has actually created this device's tunnel
	# and written config.yml; that script enables/starts the service
	# itself once that file exists (see cloudflared.initd's own comment).
fi

ln -sf /etc/init.d/local "$OVL/etc/runlevels/default/local"
for svc in wpa_supplicant dhcpcd chronyd sshd dbus avahi-daemon wetty \
	pi-bluetooth-configuration pi-relay-control victron-ve-direct; do
	ln -sf "/etc/init.d/$svc" "$OVL/etc/runlevels/default/$svc"
done
# cloudflared is deliberately NOT wired into a runlevel here -- see the
# CLOUDFLARE_API_TOKEN block above and cloudflared.initd's own comment;
# provision-cloudflare.sh enables it itself once actually configured.

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
# changes without being told to, so this was silently not taking effect
# despite the file ending up correct. `rc-service sshd restart` (not
# just editing the file) is what actually makes the new PermitRootLogin/
# PasswordAuthentication settings take effect.
#
# PermitRootLogin is "no", not "yes" -- root's own password (still set
# below, unconditionally) is only ever usable at the physical console
# now, not over SSH/Wetty at all. This is unconditional, not scoped to
# just Wetty specifically: Wetty's own --ssh-user (see wetty.initd)
# picks which account it forces a connection into, but that's a
# preference wetty applies to itself, not a security boundary a
# connecting client is bound by -- Wetty's own address() function
# (confirmed by reading its installed source directly) lets a raw HTTP
# `Remote-User` header or a `/ssh/<user>` URL path override --ssh-user
# outright, meant for sitting behind an authenticating reverse proxy
# that sets/strips that header itself, which this image doesn't run.
# With no such proxy in front of it, "just don't default wetty to root"
# would be cosmetic, not a real restriction, once Wetty is reachable
# from the internet via Cloudflare Tunnel -- only disabling root at
# sshd itself actually closes it, for every path (Wetty and direct LAN
# SSH both). pi-bluetooth-configuration's own create_admin_account()
# (see src/main.cpp, called the first time POST /finish ever succeeds)
# is the intended replacement: a per-device random username/password,
# permitted to `doas` to root, handed back to the app once, in that
# same /finish response.
ROOT_HASH=$(docker run --rm --platform linux/arm64 alpine:"$ALPINE_VERSION" sh -c \
	'apk add --no-cache openssl >/dev/null 2>&1; openssl passwd -6 "$1"' _ "$ROOT_PASSWORD")
cat > "$OVL/etc/local.d/aipicam-setup.start" <<EOF
#!/bin/sh
awk -v h='$ROOT_HASH' 'BEGIN{FS=OFS=":"} \$1=="root"{\$2=h} {print}' /etc/shadow > /etc/shadow.new
mv /etc/shadow.new /etc/shadow
chmod 640 /etc/shadow

sed -i \\
	-e 's/^#\\?PermitRootLogin.*/PermitRootLogin no/' \\
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

# COPYFILE_DISABLE=1 stops macOS's tar from polluting the archive with
# a ._<name> AppleDouble sidecar file for every single entry (its way
# of representing extended attributes in plain POSIX tar) -- harmless
# to what actually mattered so far (OpenRC's own scripts use specific
# globs like *.start, not a raw directory scan, so these were never
# actually being executed as bogus services) but still real pollution
# worth not shipping. --owner=0 --group=0 forces genuine root:root
# ownership instead of --numeric-owner preserving whatever this Mac
# account's own uid/gid happens to be -- a real device's own `lbu
# commit` (running as root) would naturally produce root:root, and nothing
# here needs to be owned by anyone else.
#
# `usr` is included alongside `etc`/`var` because cloudflared's binary
# and Wetty's bundled node_modules both live under
# usr/local/{bin,lib} -- diskless mode's root is rebuilt from nothing
# but this tarball plus the local apk repo every single boot, so
# anything under $OVL not captured here simply wouldn't exist at
# runtime. (Both directories are created unconditionally above so this
# always has something to archive, even on a build with neither
# feature's conditional content added.)
( cd "$OVL" && COPYFILE_DISABLE=1 tar czf "../../work/$PI_HOSTNAME.apkovl.tar.gz" \
	--owner=0 --group=0 etc var usr )

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
