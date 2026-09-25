#!/bin/sh
# Creates this device's own Cloudflare Tunnel and points
# <serial>.<CLOUDFLARE_DOMAIN> at it, then starts cloudflared -- invoked
# (and retried) by pi-bluetooth-configuration itself, see that repo's
# src/main.cpp provision_cloudflare_async(). $1 is this device's
# hardware serial (same identity already used for its hostname and AP
# SSID), used as both this tunnel's name and its DNS label. Only ever
# shipped/reachable at all on a build with CLOUDFLARE_API_TOKEN set --
# see build-image.sh and README.md, "Remote access via Cloudflare
# Tunnel".
#
# Unlike Tailscale's single reusable auth key (one flat mesh -- every
# device just joins the same tailnet), a Cloudflare Tunnel has no
# concept of "join": each device needs its OWN tunnel (own ID, own
# credentials, own DNS hostname), so this script does the per-device
# orchestration the Cloudflare API itself requires -- create-or-replace
# a tunnel named after this device's serial, write its credentials and
# ingress config locally, upsert the DNS record that routes to it, then
# start the service.
#
# Exit 0 means "nothing left to do" (already provisioned, or the
# feature isn't enabled on this build) -- the caller stops retrying. Any
# other exit code means "worth trying again later" (most likely: no
# internet yet, or a transient Cloudflare API error).
set -eu

SERIAL="${1:?usage: provision-cloudflare.sh <serial>}"

# Already provisioned -- config.yml is only ever written by this script,
# at the very end of a successful run below (renamed into place only
# after the DNS step also succeeds, see bottom), so its existence alone
# means there's nothing left to do. State persists across reboots the
# same way the rest of /etc does (see build-image.sh's `+etc` protected
# path and main.cpp's own `lbu commit` before reboot) -- no separate
# /var symlink onto the boot partition needed the way Tailscale's node
# identity needed one, since nothing here lives under /var.
[ -f /etc/cloudflared/config.yml ] && exit 0

# Feature not enabled on this build at all.
[ -f /etc/cloudflare-api-token ] || exit 0

API_TOKEN=$(cat /etc/cloudflare-api-token)
ACCOUNT_ID=$(cat /etc/cloudflare-account-id)
ZONE_ID=$(cat /etc/cloudflare-zone-id)
DOMAIN=$(cat /etc/cloudflare-domain)
DNS_HOSTNAME="$SERIAL.$DOMAIN"
API="https://api.cloudflare.com/client/v4"

# $1=method $2=path $3=JSON body (optional). -sS: silent progress meter
# but still print connection-level errors (e.g. no internet yet) to
# stderr, where main.cpp's own retry-attempt logging picks them up.
api() {
	if [ -n "${3:-}" ]; then
		curl -sS -X "$1" "$API$2" \
			-H "Authorization: Bearer $API_TOKEN" \
			-H "Content-Type: application/json" \
			--data "$3"
	else
		curl -sS -X "$1" "$API$2" \
			-H "Authorization: Bearer $API_TOKEN"
	fi
}

# Under `set -e`, a non-zero return here aborts the whole script right
# where it's called -- exactly the "worth trying again later" signal
# this script promises, same as a bare failing command would give.
# Printed first so main.cpp's captured output actually explains why.
check_success() {
	if ! echo "$1" | jq -e '.success == true' >/dev/null 2>&1; then
		echo "cloudflare API call failed: $(echo "$1" | jq -c '.errors // .' 2>/dev/null || echo "$1")" >&2
		return 1
	fi
}

# A tunnel found here can only be a leftover from a previous attempt
# that created it remotely but never got as far as writing config.yml
# locally (crash, power loss, reboot mid-script) -- genuinely-
# provisioned devices already exited above via the config.yml check.
# There's no way to recover such a tunnel's credentials (Cloudflare
# never returns a tunnel_secret after creation, only at the moment one
# is first set), so the only idempotent move is to delete and recreate
# it with a fresh secret rather than get permanently stuck retrying
# against a "tunnel name already exists" error forever.
EXISTING=$(api GET "/accounts/$ACCOUNT_ID/cfd_tunnel?name=$SERIAL&is_deleted=false")
check_success "$EXISTING"
EXISTING_ID=$(echo "$EXISTING" | jq -r '.result[0].id // empty')
if [ -n "$EXISTING_ID" ]; then
	echo "removing stale tunnel $EXISTING_ID from an earlier interrupted attempt" >&2
	RESP=$(api DELETE "/accounts/$ACCOUNT_ID/cfd_tunnel/$EXISTING_ID")
	check_success "$RESP"
fi

# 32 random bytes, base64-encoded -- exactly what `cloudflared tunnel
# create` itself generates client-side and sends as tunnel_secret;
# Cloudflare stores only a hash of it server-side, so this is the one
# and only moment it's ever obtainable.
TUNNEL_SECRET=$(head -c 32 /dev/urandom | base64)

CREATE_BODY=$(jq -n --arg name "$SERIAL" --arg secret "$TUNNEL_SECRET" \
	'{name: $name, tunnel_secret: $secret, config_src: "local"}')
CREATED=$(api POST "/accounts/$ACCOUNT_ID/cfd_tunnel" "$CREATE_BODY")
check_success "$CREATED"
TUNNEL_ID=$(echo "$CREATED" | jq -r '.result.id')

mkdir -p /etc/cloudflared
CREDS="/etc/cloudflared/$TUNNEL_ID.json"
jq -n --arg t "$ACCOUNT_ID" --arg s "$TUNNEL_SECRET" --arg id "$TUNNEL_ID" \
	'{AccountTag: $t, TunnelSecret: $s, TunnelID: $id}' > "$CREDS"
chmod 600 "$CREDS"

# Routed to Wetty (see wetty.initd), not raw ssh://localhost:22 --
# Wetty itself is what actually proxies to sshd (via a real `ssh`
# subprocess, --force-ssh), so this single HTTP ingress rule is enough
# to reach a full terminal from any browser at https://$DNS_HOSTNAME,
# with no cloudflared client needed on the connecting machine. Wetty
# always listens on :3000, build-time-enabled unconditionally (see
# build-image.sh) whether or not this Cloudflare feature is.
cat > /etc/cloudflared/config.yml.new <<-EOF
	tunnel: $TUNNEL_ID
	credentials-file: $CREDS
	ingress:
	  - hostname: $DNS_HOSTNAME
	    service: http://localhost:3000
	  - service: http_status:404
	EOF

# DNS: upsert rather than blind create, in case this hostname's record
# already exists from an earlier interrupted attempt (its target tunnel
# ID would be stale in that case -- either way, this run's own tunnel is
# the one that should now own it). proxied=true is load-bearing, not
# cosmetic: a *.cfargotunnel.com CNAME only actually routes traffic
# through Cloudflare's edge -- the entire mechanism a Tunnel relies on
# -- when the record itself is proxied; an unproxied ("DNS only") CNAME
# to it just fails to resolve/connect, per Cloudflare's own Tunnel
# routing docs.
DNS_TARGET="$TUNNEL_ID.cfargotunnel.com"
EXISTING_DNS=$(api GET "/zones/$ZONE_ID/dns_records?type=CNAME&name=$DNS_HOSTNAME")
check_success "$EXISTING_DNS"
DNS_ID=$(echo "$EXISTING_DNS" | jq -r '.result[0].id // empty')
DNS_BODY=$(jq -n --arg name "$DNS_HOSTNAME" --arg content "$DNS_TARGET" \
	'{type: "CNAME", name: $name, content: $content, proxied: true, ttl: 1}')
if [ -n "$DNS_ID" ]; then
	RESP=$(api PATCH "/zones/$ZONE_ID/dns_records/$DNS_ID" "$DNS_BODY")
else
	RESP=$(api POST "/zones/$ZONE_ID/dns_records" "$DNS_BODY")
fi
check_success "$RESP"

# Only commit to the real filename once the DNS side has also succeeded
# -- a failure above must NOT leave a config.yml behind, or the
# idempotency check at the top of this script would wrongly treat a
# half-finished attempt as fully done on the next retry.
mv /etc/cloudflared/config.yml.new /etc/cloudflared/config.yml

rc-update add cloudflared default
rc-service cloudflared start

# Same reasoning as provision-tailscale.sh deleting
# /etc/tailscale-authkey: this token is only ever needed for this
# device's initial provisioning, and it's considerably MORE powerful
# than Tailscale's reusable auth key was -- it can create/delete tunnels
# and DNS records account/zone-wide, not just register one more node on
# an already-scoped tailnet -- so removing it here matters even more. A
# device compromised after this point can't use it to touch your
# Cloudflare account at all; this device's own tunnel identity is
# unaffected either way, since it already persisted itself into
# config.yml/the credentials file above.
rm -f /etc/cloudflare-api-token
lbu commit -d mmcblk0p1

echo "provisioned cloudflare tunnel $TUNNEL_ID for $DNS_HOSTNAME"
