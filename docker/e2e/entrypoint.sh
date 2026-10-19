#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
#
# Container entrypoint of the e2e image.  Two phases:
#
#  1. As root, with only the capabilities the container was given
#     (NET_ADMIN to create the tap; SETUID/SETGID to be able to drop
#     privileges, see docs/features/NETWORKING_E2E.md): create tap0 owned by the
#     e2e user and give it its address.  This is the ONLY privileged step.
#  2. exec into setpriv: uid/gid 2010, no supplementary groups, no_new_privs.
#     Leaving uid 0 empties the permitted and effective capability sets (and
#     no_new_privs stops execve regaining any); e2e.py asserts CapPrm/CapEff are
#     0 before it starts anything.  (setpriv --bounding-set would need CAP_SETPCAP
#     as well, one more capability for nothing.)  No root process remains.
#
# usage: e2e-entrypoint RUNG       (one container = one rung: tap0's address is per rung)
# Environment:
#   E2E_TAP_IP    tap0 address/prefix   (default: the rung's, from e2e.py)
#   E2E_TAP_MAC   tap0 MAC              (default 02:00:00:00:00:0a)
#   E2E_TIMEOUT   hard time limit in seconds for the whole run (default 1500)
set -eu
[ $# -eq 1 ] || { echo "usage: e2e-entrypoint RUNG   (r1 r2 r2b r3 r3b r4 r5)" >&2; exit 2; }
IP="${E2E_TAP_IP:-$(python3 -I /opt/e2e/e2e.py --tap-ip "$1")}"
MAC="${E2E_TAP_MAC:-02:00:00:00:00:0a}"

if [ "$(id -u)" != 0 ]; then
    echo "entrypoint: must start as root (needs CAP_NET_ADMIN once, then drops to uid 2010)" >&2
    exit 1
fi
[ -c /dev/net/tun ] || { echo "entrypoint: /dev/net/tun missing (--device /dev/net/tun)" >&2; exit 1; }

ip tuntap add dev tap0 mode tap user 2010 group 2010
ip link set tap0 address "$MAC"
ip addr add "$IP" dev tap0
ip link set tap0 up
# Two host routes, because this namespace has no default route (--network none):
#  - PeerTalk's POSIX backend finds its own address by connect()ing a UDP socket to
#    8.8.8.8 (no packet is sent) and reading the local address; without a route it
#    falls back to 127.0.0.1 and never announces itself on the LAN;
#  - it broadcasts discovery to 255.255.255.255, which Linux refuses (ENETUNREACH)
#    when no route matches, so nothing would leave tap0.
# Nothing else is routed: tap0 is a port of the closed hub, there is no other network.
ip route add 8.8.8.8/32 dev tap0
ip route add 255.255.255.255/32 dev tap0
sysctl -qw net.ipv6.conf.tap0.disable_ipv6=1 2>/dev/null || true
echo "entrypoint: tap0 $IP mac $MAC; dropping to uid 2010"

exec setpriv --reuid=2010 --regid=2010 --clear-groups \
    --no-new-privs \
    timeout -k 10 "${E2E_TIMEOUT:-1500}" python3 -I /opt/e2e/e2e.py "$@"
