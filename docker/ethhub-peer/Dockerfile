# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
#
# ethhub peer: a Linux container that runs ethhub with a TAP port (tap0), gives
# tap0 a static IP, and then runs a POSIX peer command on that LAN.
#
# Build: docker/ethhub-peer/build.sh   (tar context of 4 files), or from the repo root:
#   docker build -t ethhub-peer -f docker/ethhub-peer/Dockerfile .
# Run: see docs/features/ETHHUB.md and selftest.sh
#   docker run --rm --cap-add NET_ADMIN --device /dev/net/tun \
#       -p 127.0.0.1:5555:5555/udp ethhub-peer [peer command...]
# Publish on 127.0.0.1 only: the hub has no authentication and floods every
# broadcast to all clients, so a hub reachable from the network is a UDP
# reflector.  Inside the container the hub listens on 0.0.0.0 (ETHHUB_LISTEN),
# which is what makes Docker's port publishing and other containers work.
#
# Privileges: the entrypoint starts as root only to create tap0 and set its
# address (the one thing that needs NET_ADMIN), then runs ethhub and the peer
# command as the unprivileged user "ethhub" (no capabilities).

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base linux-headers
COPY tools/ethhub/ethhub.c /src/ethhub.c
RUN cc -std=c11 -O2 -Wall -Wextra -Werror -o /ethhub /src/ethhub.c && strip /ethhub

FROM alpine:3.20
# iproute2 for `ip`, python3 only for the bundled test peer (netecho.py).
RUN apk add --no-cache iproute2 python3 su-exec \
    && adduser -D -H -u 10001 ethhub
COPY --from=build /ethhub /usr/local/bin/ethhub
COPY docker/ethhub-peer/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY docker/ethhub-peer/netecho.py /usr/local/bin/netecho.py
COPY docker/ethhub-peer/fakeclient.py /usr/local/bin/fakeclient.py
ENV ETHHUB_IP=10.88.0.10/24 \
    ETHHUB_PORT=5555 \
    ETHHUB_USER=ethhub
EXPOSE 5555/udp
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
# Default peer: UDP-broadcast listener + TCP echo on port 7000
CMD ["python3", "/usr/local/bin/netecho.py", "serve", "7000"]
