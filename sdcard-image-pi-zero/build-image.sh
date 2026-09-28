#!/bin/bash
# Builds a bootable Raspberry Pi Zero / Zero W (ARMv6, Alpine's "armhf")
# SD card image -- same diskless-mode approach as ../sdcard-image-pi3,
# just targeting a different architecture. See that directory's
# build-image.sh for the full rationale of diskless mode itself (tmpfs
# root, no fsck/root/localmount step to get wrong); this file only
# documents what's actually DIFFERENT for the Zero.
#
# Alpine's own "armhf" arch is ARMv6 with hardware float -- exactly what
# the Zero/Zero W's BCM2835 needs (NOT "armv7", which targets the
# Cortex-A7+ SoCs in a Pi 2 and later, and would refuse to even boot
# here). Confirmed directly against Alpine's own release server, not
# assumed. This also means -- unlike Wetty, a Node.js/V8 program that
# hasn't properly supported ARMv6 in years, the specific risk that led
# to replacing it with ttyd project-wide -- everything shipped here is
# either Alpine's own official armhf apk builds or a plain C binary
# (cloudflared), so there's no equivalent risk left to design around.
#
# See README.md for what else is Zero-specific (no onboard Ethernet,
# single USB OTG port, WiFi-only from first boot).
set -euo pipefail
cd "$(dirname "$0")"

ALPINE_VERSION=3.22
ALPINE_RELEASE=3.22.5
# Same pinned-together reasoning as sdcard-image-pi3/build-image.sh.
# SHA256 is GitHub's own per-asset digest for cloudflared-linux-armhf at
# this exact tag (confirmed directly against the releases API, not
# assumed) -- a DIFFERENT asset and digest than the pi3 build's aarch64
# one, despite sharing the same version number.
CLOUDFLARED_VERSION=2026.9.3
CLOUDFLARED_SHA256=a714b1bee87e71ce7260555b30722ce711d12aaa0fee5a8aadc767ee6b816a14
PI_HOSTNAME="${PI_HOSTNAME:-aipicam}"
IMG_SIZE_MB=768
OUT_IMG="aipicam-pi-zero-diskless.img"

BT_APK="artifacts/pi-bluetooth-configuration-armhf.apk"
RELAY_APK="artifacts/pi-relay-control-armhf.apk"
VICTRON_APK="artifacts/victron-ve-direct-armhf.apk"
[ -f "$BT_APK" ] || { echo "missing $BT_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$RELAY_APK" ] || { echo "missing $RELAY_APK -- see README.md for how to fetch it" >&2; exit 1; }
[ -f "$VICTRON_APK" ] || { echo "missing $VICTRON_APK -- see README.md for how to fetch it" >&2; exit 1; }

: "${ROOT_PASSWORD:?set ROOT_PASSWORD in the environment (used once, at build time, to hash into /etc/shadow -- never stored in plaintext or committed)}"

# Optional: Cloudflare Tunnel -- see sdcard-image-pi3/build-image.sh's
# own comment on this same block for the full rationale (per-device
# tunnel, not a shared secret). Identical here, just a different
# cloudflared binary further down.
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
mkdir -p work/bootfs work/repo/armhf work/apkovl

# ── 1. Fetch Alpine's own official diskless release ─────────────────────────
# Same official-bundle reasoning as the pi3 build -- "armhf" here, not
# "armv7" (see the header comment above for why that distinction
# matters on a Zero specifically).
echo "==> Fetching official alpine-rpi-$ALPINE_RELEASE-armhf.tar.gz"
OFFICIAL_TARBALL="work/alpine-rpi-$ALPINE_RELEASE-armhf.tar.gz"
if [ ! -f "$OFFICIAL_TARBALL" ]; then
	curl -fsSL -o "$OFFICIAL_TARBALL" \
		"https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/armhf/alpine-rpi-${ALPINE_RELEASE}-armhf.tar.gz"
fi
tar -C work/bootfs -xzf "$OFFICIAL_TARBALL"

# ── 2. Build an extended, fully-offline-capable local apk repository ───────
# Same reasoning as the pi3 build; --platform linux/arm/v6 (not arm64)
# is what actually gets genuine armhf/ARMv6 binaries out of Docker's
# multi-arch alpine image here -- confirmed directly against its own
# manifest (linux/arm/v6 is a real listed platform), not assumed.
echo "==> Fetching additional packages (offline dependency closure)"
cp work/bootfs/apks/armhf/*.apk work/repo/armhf/
CLOUDFLARE_PACKAGES=""
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	CLOUDFLARE_PACKAGES="curl jq"
fi
docker run --rm --platform linux/arm/v6 -v "$PWD/work/repo/armhf":/out alpine:"$ALPINE_VERSION" sh -c '
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
		ttyd openssh-client \
		doas shadow \
		'"$CLOUDFLARE_PACKAGES"'
'
cp "$BT_APK" "$RELAY_APK" "$VICTRON_APK" work/repo/armhf/

# Same rename-to-real-pkgver-filename step as the pi3 build -- see its
# own comment for why apk needs this.
for pkg in pi-bluetooth-configuration pi-relay-control victron-ve-direct; do
	ver=$(docker run --rm -v "$PWD/work/repo/armhf":/repo:ro alpine:"$ALPINE_VERSION" \
		sh -c "tar -xzOf /repo/$pkg-armhf.apk .PKGINFO 2>/dev/null | awk -F' = ' '/^pkgver/{print \$2}'")
	mv "work/repo/armhf/$pkg-armhf.apk" "work/repo/armhf/$pkg-$ver.apk"
done

echo "==> Building and signing the local repository index"
# --rewrite-arch armhf, not aarch64 -- same "noarch" rewrite reasoning as
# the pi3 build, just this repo's actual arch instead.
docker run --rm --platform linux/arm/v6 -v "$PWD/work/repo":/repo alpine:"$ALPINE_VERSION" sh -c '
	set -e
	apk add --no-cache alpine-sdk >/dev/null
	abuild-keygen -a -n -q
	KEY=$(ls ~/.abuild/*.rsa | head -1)
	cd /repo/armhf
	apk index --allow-untrusted --rewrite-arch armhf -o APKINDEX.unsigned.tar.gz *.apk
	abuild-sign -k "$KEY" APKINDEX.unsigned.tar.gz
	mv APKINDEX.unsigned.tar.gz APKINDEX.tar.gz
	cp "${KEY}.pub" "/repo/$(basename "${KEY}.pub")"
'
REPO_KEY=$(basename work/repo/*.rsa.pub)

# ── 3. Build the apkovl overlay ─────────────────────────────────────────────
# Identical in content and reasoning to the pi3 build -- nothing in this
# section is architecture-specific.
echo "==> Building the apkovl overlay"
OVL=work/apkovl
mkdir -p "$OVL"/etc/apk/keys "$OVL"/etc/apk/protected_paths.d \
	"$OVL"/etc/runlevels/boot "$OVL"/etc/runlevels/default "$OVL"/etc/runlevels/shutdown \
	"$OVL"/etc/init.d "$OVL"/etc/local.d "$OVL"/etc/doas.d "$OVL"/var/lib \
	"$OVL"/usr/local/bin

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
	ttyd
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

cat > "$OVL/etc/apk/protected_paths.d/lbu.list" <<-EOF
	+etc
EOF

echo "/media/mmcblk0p1/apks" > "$OVL/etc/apk/repositories"

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

cp wait-for-wlan.initd "$OVL/etc/init.d/wait-for-wlan"
chmod +x "$OVL/etc/init.d/wait-for-wlan"
ln -sf /etc/init.d/wait-for-wlan "$OVL/etc/runlevels/boot/wait-for-wlan"

cp restore-wifi-config.initd "$OVL/etc/init.d/restore-wifi-config"
chmod +x "$OVL/etc/init.d/restore-wifi-config"
ln -sf /etc/init.d/restore-wifi-config "$OVL/etc/runlevels/boot/restore-wifi-config"

cp fix-chrony-makestep.initd "$OVL/etc/init.d/fix-chrony-makestep"
chmod +x "$OVL/etc/init.d/fix-chrony-makestep"
ln -sf /etc/init.d/fix-chrony-makestep "$OVL/etc/runlevels/boot/fix-chrony-makestep"

cp ttyd.initd "$OVL/etc/init.d/ttyd"
chmod +x "$OVL/etc/init.d/ttyd"

# Cloudflare Tunnel -- same per-device orchestration as the pi3 build,
# just the armhf cloudflared binary (a real ARMv6 build Cloudflare
# publishes directly, confirmed against this exact release's own asset
# list -- not the "arm" asset, which targets ARMv7+ instead and won't
# run on this SoC).
if [ -n "$CLOUDFLARE_API_TOKEN" ]; then
	echo "==> Fetching cloudflared $CLOUDFLARED_VERSION (armhf)"
	mkdir -p "$OVL/usr/local/bin"
	CLOUDFLARED_BIN="$OVL/usr/local/bin/cloudflared"
	curl -fsSL -o "$CLOUDFLARED_BIN" \
		"https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-armhf"
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

	printf '%s' "$CLOUDFLARE_API_TOKEN" > "$OVL/etc/cloudflare-api-token"
	printf '%s' "$CLOUDFLARE_ACCOUNT_ID" > "$OVL/etc/cloudflare-account-id"
	printf '%s' "$CLOUDFLARE_ZONE_ID" > "$OVL/etc/cloudflare-zone-id"
	printf '%s' "$CLOUDFLARE_DOMAIN" > "$OVL/etc/cloudflare-domain"
	chmod 600 "$OVL/etc/cloudflare-api-token" "$OVL/etc/cloudflare-account-id" \
		"$OVL/etc/cloudflare-zone-id" "$OVL/etc/cloudflare-domain"
fi

ln -sf /etc/init.d/local "$OVL/etc/runlevels/default/local"
for svc in wpa_supplicant dhcpcd chronyd sshd dbus avahi-daemon ttyd \
	pi-bluetooth-configuration pi-relay-control victron-ve-direct; do
	ln -sf "/etc/init.d/$svc" "$OVL/etc/runlevels/default/$svc"
done

# Same root-lockdown/admin-account handoff as the pi3 build -- see its
# own comment for the full reasoning. --platform linux/arm/v6 here only
# matters for reproducing the exact same `openssl passwd` output a real
# Zero would produce; in practice this hash is arch-independent, but
# there's no reason to mix an amd64/arm64 host binary in here either.
ROOT_HASH=$(docker run --rm --platform linux/arm/v6 alpine:"$ALPINE_VERSION" sh -c \
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

ln -sf /media/mmcblk0p1/relay-state "$OVL/var/lib/relay_control"

( cd "$OVL" && COPYFILE_DISABLE=1 tar czf "../../work/$PI_HOSTNAME.apkovl.tar.gz" \
	--owner=0 --group=0 etc var usr )

# ── 4. Assemble the boot media contents ─────────────────────────────────────
echo "==> Assembling boot media contents"
rm -rf work/bootfs/apks/armhf
cp -r work/repo/armhf work/bootfs/apks/armhf
cp "work/repo/$REPO_KEY" work/bootfs/apks/
touch work/bootfs/apks/.boot_repository
cp "work/$PI_HOSTNAME.apkovl.tar.gz" work/bootfs/
mkdir -p work/bootfs/relay-state

# ── 5. Build the final single-partition .img ────────────────────────────────
echo "==> Building the final .img"
docker run --rm --platform linux/arm/v6 -v "$PWD/work":/work alpine:"$ALPINE_VERSION" sh -c '
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
