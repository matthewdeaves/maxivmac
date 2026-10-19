#!/bin/sh
# Proves the virtual LAN end to end with plain `docker` (no compose, no host UDP
# port forwarding needed): ARP/ICMP/UDP-broadcast from a fake client and TCP
# between two TAP-backed containers.  Run from the repo root.
set -eu
IMG=ethhub-peer
NET=ethhub-selftest
trap 'docker rm -f ethhub-p1 ethhub-p2 >/dev/null 2>&1; docker network rm $NET >/dev/null 2>&1' EXIT

"$(dirname "$0")/build.sh" $IMG >/dev/null 2>&1
docker network create $NET >/dev/null
RUN="docker run -d --network $NET --cap-add NET_ADMIN --device /dev/net/tun"
$RUN --name ethhub-p1 -p 5555:5555/udp -e ETHHUB_IP=10.88.0.10/24 $IMG >/dev/null
$RUN --name ethhub-p2 -e ETHHUB_IP=10.88.0.20/24 -e ETHHUB_CONNECT=ethhub-p1:5555 $IMG \
    sleep 600 >/dev/null
sleep 3
FC="docker run --rm --network $NET --entrypoint python3 $IMG -I /usr/local/bin/fakeclient.py --hub ethhub-p1:5555"

echo "== ARP + ICMP echo from fake client (02:00:00:00:00:20 / 10.88.0.30) to 10.88.0.10"
$FC ping 10.88.0.10
echo "== UDP broadcast 10.88.0.255:7000 from fake client, received in container"
$FC bcast 7000 hello-bcast
docker logs ethhub-p1 2>&1 | grep '^UDP from'
echo "== TCP echo 10.88.0.20 -> 10.88.0.10 over the hub (two containers)"
docker exec ethhub-p2 python3 /usr/local/bin/netecho.py tcp 10.88.0.10 7000 hello-tcp
echo "== ping 10.88.0.10 from peer2"
docker exec ethhub-p2 ping -c 2 -W 2 10.88.0.10
echo "== UDP broadcast from peer2 (10.88.0.255:7000)"
docker exec ethhub-p2 python3 /usr/local/bin/netecho.py bcast 10.88.0.255 7000 from-peer2
