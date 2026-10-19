#!/bin/sh
# Starts ethhub with a TAP port, configures tap0, then runs the peer command.
#   ETHHUB_IP       tap0 address/prefix        (default 10.88.0.10/24)
#   ETHHUB_MAC      tap0 MAC                   (default 02:00:00:00:00:<last octet in hex>)
#   ETHHUB_PORT     UDP listen port            (default 5555)
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

# shellcheck disable=SC2086
ethhub --listen "0.0.0.0:$PORT" --tap tap0 ${ETHHUB_CONNECT:+--connect "$ETHHUB_CONNECT"} ${ETHHUB_ARGS:-} &
HUB=$!
trap 'kill $HUB 2>/dev/null || true' EXIT INT TERM

i=0
until ip link show tap0 >/dev/null 2>&1; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "entrypoint: tap0 did not appear" >&2; exit 1; }
    kill -0 $HUB 2>/dev/null || { echo "entrypoint: ethhub exited" >&2; exit 1; }
    sleep 0.1
done
ip link set tap0 address "$MAC"
ip addr add "$IP" dev tap0
ip link set tap0 up
# keep the LAN IPv4-only (avoids IPv6 RA/MLD chatter flooding every client)
sysctl -qw net.ipv6.conf.tap0.disable_ipv6=1 2>/dev/null || true
echo "entrypoint: tap0 $IP mac $MAC, hub udp/$PORT"

if [ $# -eq 0 ]; then
    wait $HUB
else
    "$@" &
    PEER=$!
    wait $PEER
fi
