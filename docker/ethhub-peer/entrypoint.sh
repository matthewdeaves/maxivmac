#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
# Starts ethhub with a TAP port, configures tap0, then runs the peer command.
#   ETHHUB_IP       tap0 address/prefix        (default 10.88.0.10/24)
#   ETHHUB_MAC      tap0 MAC                   (default 02:00:00:00:00:<last octet in hex>)
#   ETHHUB_PORT     UDP listen port            (default 5555)
#   ETHHUB_LISTEN   ethhub --listen address    (default 0.0.0.0:$ETHHUB_PORT: inside the
#                   container; publish the port on 127.0.0.1 only, see the Dockerfile)
#   ETHHUB_ALLOW    space-separated CIDRs allowed to join (ethhub --allow), default any
#   ETHHUB_USER     unprivileged user for ethhub and the peer (default ethhub);
#                   "root" keeps everything privileged (e.g. a peer that needs raw sockets)
#   ETHHUB_CONNECT  host:port of another ethhub to join as an uplink (optional)
#   ETHHUB_ARGS     extra ethhub arguments, e.g. "--verbose --pcap /tmp/x.pcap"
set -eu
IP="${ETHHUB_IP:-10.88.0.10/24}"
PORT="${ETHHUB_PORT:-5555}"
LAST="${IP%%/*}"; LAST="${LAST##*.}"
MAC="${ETHHUB_MAC:-$(printf '02:00:00:00:00:%02x' "$LAST")}"

if [ ! -c /dev/net/tun ]; then
    echo "entrypoint: /dev/net/tun missing; run with --device /dev/net/tun --cap-add NET_ADMIN" >&2
    exit 1
fi

USER_="${ETHHUB_USER:-ethhub}"
if [ "$USER_" = root ]; then DROP=""; else DROP="su-exec $USER_"; fi
ALLOW=""
for c in ${ETHHUB_ALLOW:-}; do ALLOW="$ALLOW --allow $c"; done

# Create tap0 here, as root with NET_ADMIN, owned by the unprivileged user:
# ethhub can then attach to it (TUNSETIFF) without any capability.
if [ "$USER_" = root ]; then
    ip tuntap add dev tap0 mode tap
else
    ip tuntap add dev tap0 mode tap user "$USER_"
fi

# shellcheck disable=SC2086
$DROP ethhub --listen "${ETHHUB_LISTEN:-0.0.0.0:$PORT}" --tap tap0 $ALLOW \
    ${ETHHUB_CONNECT:+--connect "$ETHHUB_CONNECT"} ${ETHHUB_ARGS:-} &
HUB=$!
trap 'kill $HUB 2>/dev/null || true' EXIT INT TERM

# tap0 already exists, so there is nothing to wait for but a failed start.
sleep 0.5
kill -0 $HUB 2>/dev/null || { echo "entrypoint: ethhub exited" >&2; exit 1; }
ip link set tap0 address "$MAC"
ip addr add "$IP" dev tap0
ip link set tap0 up
# keep the LAN IPv4-only (avoids IPv6 RA/MLD chatter flooding every client)
sysctl -qw net.ipv6.conf.tap0.disable_ipv6=1 2>/dev/null || true
echo "entrypoint: tap0 $IP mac $MAC, hub udp/$PORT, running as $USER_"

if [ $# -eq 0 ]; then
    wait $HUB
else
    $DROP "$@" &
    PEER=$!
    wait $PEER
fi
