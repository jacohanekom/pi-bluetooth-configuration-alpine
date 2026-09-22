#!/bin/sh
# Joins this device to the tailnet, the first time it has both internet
# access and hasn't already succeeded -- invoked (and retried) by
# pi-bluetooth-configuration itself, see that repo's src/main.cpp
# provision_tailscale_async(). $1 is this device's hardware serial (same
# identity already used for its hostname and AP SSID), used as this
# device's Tailscale hostname too. Only ever shipped/reachable at all on
# a build with TAILSCALE_AUTHKEY set -- see build-image.sh and
# README.md, "Remote access via Tailscale".
#
# Exit 0 means "nothing left to do" (already joined, or the feature
# isn't enabled on this build) -- the caller stops retrying. Any other
# exit code means "worth trying again later" (most likely: no internet
# yet).
set -eu

SERIAL="${1:?usage: provision-tailscale.sh <serial>}"

# Already joined -- plain `tailscale status` (unlike --json, which
# always exits 0 and reports state in its BackendState field instead)
# reliably exits 1 with "Logged out." when this device hasn't
# authenticated yet, and 0 once it has -- confirmed directly against the
# real CLI, not assumed. State persists across reboots via
# /var/lib/tailscale's own symlink onto the boot partition (see
# build-image.sh), so this correctly skips on every boot after the
# first successful join.
tailscale status >/dev/null 2>&1 && exit 0

# Feature not enabled on this build at all.
[ -f /etc/tailscale-authkey ] || exit 0

AUTHKEY=$(cat /etc/tailscale-authkey)

if ! tailscale up --authkey="$AUTHKEY" --hostname="$SERIAL"; then
	echo "tailscale up failed (no internet yet?)" >&2
	exit 1
fi

# Reusable though this key is, it's only ever needed for this device's
# initial join -- deleting it means a device compromised *after* this
# point can't use it to register additional rogue devices on the
# tailnet. The deletion itself needs committing (same as any other
# change under /etc) or the original build-time apkovl -- still holding
# the key -- would just reapply it on the next boot, undoing this
# entirely; this device's own Tailscale identity is unaffected either
# way, since that already persisted itself via the /var/lib/tailscale
# symlink the moment `tailscale up` above wrote its state file.
rm -f /etc/tailscale-authkey
lbu commit -d mmcblk0p1

echo "joined tailnet as $SERIAL"
