# ethhub: virtual LAN hub for the emulator

`tools/ethhub/ethhub.c` is a small Ethernet hub/switch that talks to its ports
over UDP. It exists so that one or more maxivmac instances (using the
DaynaPORT SCSI Ethernet adapter, see `NETWORKING_DAYNAPORT.md`) and a POSIX
peer (PeerTalk) can share one real layer-2 LAN, including UDP broadcast and TCP, without root on the host.

Status: the emulator joins a hub with `--eth=hub:HOST:PORT` (`EthHub` in
`src/devices/eth_hub.{h,cpp}`, see `NETWORKING_DAYNAPORT.md`), alongside
`--eth=slirp`.

## Wire format (fixed; do not extend)

- Exactly **one raw Ethernet frame per UDP datagram**, both directions.
- 14..1514 bytes: dst MAC, src MAC, ethertype, payload. No FCS, no extra header.
- A **zero-length datagram** is a hello/keepalive: it registers the sender as a
  client (an emulator may have nothing to send yet) and refreshes its timeout.
- A client is identified by its UDP source address (IPv4 address + port).
- Datagrams of 1..13 bytes or more than 1514 bytes are dropped and counted.

## Behaviour

- Learns source MAC -> client (multicast/broadcast sources are never learned).
- Unicast to a learned MAC goes to that client only; broadcast, multicast and
  unknown unicast flood to every other port. A frame is never sent back to the
  port it came from.
- Clients age out after `--client-timeout` seconds (default 60) without any
  datagram (a hello is enough); MACs after `--mac-timeout` (default 300).
  Ageing granularity is one second, so a timeout of N fires between N-1 and N
  seconds. Clients must therefore send a keepalive well inside the timeout
  (every 10-20 s is plenty).
- Tables are bounded: `--max-clients` (64, max 4096): new clients beyond the
  limit are ignored and counted (`dropped_full`); `--max-macs` (1024, max
  4096; lookups are linear scans): the least recently seen MAC is evicted.
- Counters are printed to stderr on SIGUSR1 and on exit (SIGINT/SIGTERM).
- `--verbose` logs client joins/leaves and one line per frame.
- `--pcap FILE` writes every frame the hub receives (ingress) as a standard
  pcap file (Ethernet linktype), flushed per frame; open it in Wireshark.
- `--tap NAME` (Linux only; compiled out elsewhere) attaches a TAP device as
  one more port, so a local Linux IP stack is on the LAN. Needs `/dev/net/tun`
  and `NET_ADMIN`, i.e. a container.
- `--connect host:port` joins another ethhub as a pinned client (hello every
  5 s, never ages out). Used to chain hubs. There is no spanning tree: hubs
  must form a tree, never a loop.
- `--listen [addr:]port` (default **`127.0.0.1:5555`**; port 0 = ephemeral,
  the chosen port is printed as `ethhub listening on N` on stdout, the bound
  address as `ethhub: bound to A:P` on stderr). Anything but loopback needs an
  explicit address, e.g. `--listen 0.0.0.0:5555`, and prints a warning.
- `--allow CIDR` (repeatable, `a.b.c.d[/n]`): only datagrams from these
  sources are accepted; others are counted (`dropped_denied`).

### Security

There is no authentication. Any datagram from any source that reaches the
socket makes that source a client, and every broadcast is flooded to all
clients. A hub that listens beyond loopback can therefore be used for UDP
reflection and amplification (spoofed source addresses), lets anyone who can
reach it read the LAN's broadcast traffic, and lets them claim a MAC. Hence
the loopback default, the warning, and `--allow`. There is no per-client
rate limit (deferred); do not expose a hub to the internet.

Single-threaded `poll()` loop; plain C11 + POSIX, no dependencies. Runs on
macOS and Linux.

## Build and test

- CMake: `-DMAXIVMAC_ETHHUB=ON` (default on UNIX) adds targets `ethhub` and
  `ethhub-asan`; `ctest -R ethhub` runs `tools/ethhub/test/test_ethhub.py`
  against the ASan/UBSan binary (about 25 s; includes registration, flooding,
  unicast, ageing, size limits, table bounds, pcap, uplink, the loopback
  default and `--allow`, a 6000-datagram random fuzz).
- Standalone: `make -C tools/ethhub` (add `SAN=1` for sanitizers), or just
  `cc -std=c11 -O2 -o ethhub tools/ethhub/ethhub.c`.
- Manual: `python3 -I tools/ethhub/test/test_ethhub.py ./ethhub`.

## Docker peer (`docker/ethhub-peer/`)

A Linux container runs `ethhub --tap tap0`, gives `tap0` a static IP, then
runs a peer command on that LAN.

```
docker/ethhub-peer/build.sh                       # builds image "ethhub-peer" (~80 MB, Alpine)
docker run --rm --cap-add NET_ADMIN --device /dev/net/tun \
    -p 127.0.0.1:5555:5555/udp ethhub-peer        # default peer: netecho serve 7000
docker run ... ethhub-peer <your peer command>    # any other peer
docker/ethhub-peer/selftest.sh                    # the proofs below, end to end
```

Environment: `ETHHUB_IP` (default `10.88.0.10/24`), `ETHHUB_MAC` (default
`02:00:00:00:00:<last octet>`), `ETHHUB_PORT` (5555), `ETHHUB_LISTEN`
(default `0.0.0.0:$ETHHUB_PORT`), `ETHHUB_ALLOW` (space-separated CIDRs for
`--allow`), `ETHHUB_CONNECT` (`host:port` of another hub), `ETHHUB_USER`
(default `ethhub`), `ETHHUB_ARGS` (extra ethhub flags, e.g.
`--verbose --pcap /tmp/lan.pcap`).

Inside the container the hub listens on 0.0.0.0, which Docker's port
publishing and other containers need; **publish with
`-p 127.0.0.1:5555:5555/udp`** (never a bare `-p 5555:5555/udp`, which binds
every host interface) and see Security above. Privileges: the entrypoint runs
as root with `NET_ADMIN` only to create `tap0` (owned by the `ethhub` user)
and set its address, then runs ethhub and the peer command as the
unprivileged `ethhub` user with no capabilities (`ETHHUB_USER=root` opts
out, e.g. for a peer that needs raw sockets; `docker exec` still gives root).

- `netecho.py`: the bundled test peer: UDP broadcast listener (replies by
  unicast) plus TCP echo on one port; also `bcast` and `tcp` client modes.
- `fakeclient.py`: host-side fake emulator speaking ARP, ICMP echo and UDP
  broadcast over the hub framing (stdlib only).
- `selftest.sh` runs everything on a private Docker network, so it does not
  depend on host UDP port forwarding.

### How PeerTalk will plug in

`/Users/matt/Documents/peertalk/docker/Dockerfile.posix` is a plain
`ubuntu:22.04` build/test image (build-essential, python, lcov, cppcheck; code
mounted at `/workspace`, `CMD /bin/bash`); it has no networking set-up. The
PeerTalk peer image will be a new image that adds `iproute2`, builds
`ethhub.c` with `cc` (do not copy the Alpine/musl binary into a glibc image),
copies `docker/ethhub-peer/entrypoint.sh`, sets it as `ENTRYPOINT`, and uses
the PeerTalk POSIX peer binary as `CMD`. The entrypoint gives the peer a
`tap0` interface and an address on 10.88.0.0/24; PeerTalk's UDP broadcast
discovery (to 10.88.0.255) and TCP then work between that peer and any
emulator joined to the hub.

## Proofs run (Colima/Docker 29.5.2, Linux VM, aarch64)

From `selftest.sh` (fake client 02:00:00:00:00:20 / 10.88.0.30 in a container
on the same Docker network as the hub container):

```
ARP reply: 10.88.0.10 is-at 02:00:00:00:00:0a
ICMP echo reply from 10.88.0.10, 61 bytes, 0.1 ms
UDP reply from 10.88.0.10:7000 -> b'ack:hello-bcast'     (broadcast to 10.88.0.255:7000)
echo: hello-tcp                                          (TCP 10.88.0.20 -> 10.88.0.10)
64 bytes from 10.88.0.10: seq=0 ttl=64 time=1.110 ms     (ping, container 2 -> container 1)
```

## Limits and caveats

- **Known abuse limits** (measured against a hub on loopback with scripts;
  no code change, the protection is the loopback default and `--allow`):
  - *Client table exhaustion*: 64 datagrams from 64 other source ports fill the
    default table (`--max-clients`), after which a new legitimate client is
    refused until entries expire (`--client-timeout`).
  - *Amplification*: one 1514-byte broadcast is sent to every other client,
    63 copies (about 95 KB) with 64 clients. With a spoofed UDP source address
    a hub reachable from outside is a reflector.
  - *MAC hijack*: the hub learns the source MAC of any frame, so a client that
    sends a frame with another client's MAC as source takes over that MAC's
    unicast traffic until the entry is relearned or expires.
  - *Client table is not authenticated or rate limited* (also see Security).
  - A fuzz run of random and truncated datagrams (several seeds, ASan build)
    found no crash or sanitizer report.
- **MTU**: frames above 1514 bytes (1500 MTU) are dropped; UDP datagrams are
  limited by the host path too. No fragmentation or jumbo support. A TAP
  device must stay at MTU 1500.
- **IPv4 only** for the hub socket. No IPv6 transport, no VLANs, no STP.
- **UDP source identity**: behind NAT (Docker port publishing) the hub sees the
  NAT address/port; replies go back to it, which works while the mapping is
  alive. Send a keepalive periodically so the mapping and the hub entry both
  survive.
- **Docker UDP port publishing on this machine (Colima, `portForwarder:
  ssh`)**: published TCP ports work from the macOS host, **published UDP
  ports do not**: a datagram sent from the macOS host to
  `127.0.0.1:5555` never reaches the container (hub logged no client), while
  the same datagram sent from inside the Linux VM does. This is the SSH
  forwarder, which does not carry UDP; Colima's `--port-forwarder grpc` or
  `--network-address` (and Docker Desktop, which forwards UDP) are the ways
  around it. I did not change the user's Colima configuration, so a
  **host-to-container run of the fake client was not performed**; the proofs
  above run the client in a container on the same network instead.
- Also with Docker Desktop/Colima there is no route from the macOS host to
  container IPs, so only the published UDP port is reachable from outside.
- The TAP interface keeps an IPv6 link-local address (the sysctl to remove it
  is not writable in the container), so a little IPv6 ND/MLD traffic floods
  the LAN. Harmless for the Mac guest.
- Performance (Linux container, loopback, Python clients, so client-bound):
  one-way latency through the hub median 74 us / p99 327 us; 170k pps offered,
  98.8 % delivered with the hub not saturated. On the macOS host loopback
  here, Python could only send about 3k pps even to a plain sink socket, so no
  meaningful macOS number was taken. Frame lookup is linear in table size
  (fine for the default limits).

## QEMU compatibility (NOT verified)

QEMU's UDP/dgram network backends (`-netdev dgram,...` and the older
`-netdev socket,udp=...`/`-net socket,udp=`) are documented to carry one raw
Ethernet frame per datagram with no extra header, which is what ethhub uses,
and QEMU would join by sending to the hub's address. This has **not** been
tested here: no QEMU was run against ethhub. In particular the hello/keepalive
(zero-length datagram) is an ethhub convention that QEMU does not emit, so a
QEMU endpoint only becomes known to the hub when it first sends a frame, and
it would also age out after the client timeout unless it keeps sending; use a
long `--client-timeout` or a small helper that sends keepalives.
