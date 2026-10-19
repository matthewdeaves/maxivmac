# ethhub: virtual LAN hub for the emulator

`tools/ethhub/ethhub.c` is a small Ethernet hub/switch that talks to its ports
over UDP. It exists so that one or more maxivmac instances (using the
DaynaPORT SCSI Ethernet adapter, see `NETWORKING_DAYNAPORT.md`) and a POSIX
peer (PeerTalk) can share one real layer-2 LAN, including UDP broadcast and TCP, without root on the host.

Status: the emulator joins a hub with `--eth=hub:HOST:PORT` (`EthHub` in
`src/devices/eth_hub.{h,cpp}`, see `NETWORKING_DAYNAPORT.md`), alongside
`--eth=slirp`.

## Wire format (fixed; the only addition is the optional keyed hello)

- Exactly **one raw Ethernet frame per UDP datagram**, both directions.
- 14..1514 bytes: dst MAC, src MAC, ethertype, payload. No FCS, no extra header.
- A **zero-length datagram** is a hello/keepalive: it registers the sender as a
  client (an emulator may have nothing to send yet) and refreshes its timeout.
- A client is identified by its UDP source address (IPv4 address + port).
- Datagrams of 1..13 bytes or more than 1514 bytes are dropped and counted.
  The one exception is the optional **keyed hello** (13 bytes, see
  Authentication), which only a hub started with `--key-file` looks at; a hub
  without a key drops it as "short", exactly as before. Without `--key-file`
  the wire is byte-for-byte what it always was, and a QEMU dgram endpoint
  still works.

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
- Tables are bounded: `--max-clients` (64, max 4096) and `--max-macs` (1024,
  max 4096). Both are chained hash tables (seeded hash, O(1) lookup) with an
  LRU list: a full MAC table evicts the least recently seen MAC in O(1), and
  ageing only visits what expires.
- A full client table evicts the least recently seen client **if it has been
  idle for `--live-window`** (default 15 s, three keepalives); otherwise the
  newcomer is refused and counted (`dropped_full`). A client that keeps its
  hellos going is therefore never pushed out by newcomers, and a table full of
  dead clients does not lock the LAN for `--client-timeout`. One source IP can
  hold at most `--max-per-ip` clients (default 16 for other hosts, **no limit
  for loopback** unless `--max-per-ip` is given, so any number of local
  emulators can join; 0 = no limit everywhere; counted as `dropped_ip_cap`,
  and a refusal is logged once per 10 s). Mind NAT: everything behind one
  NAT/Docker gateway is one IP, so raise or zero the limit there.
- **MAC pinning** (`--mac-move=deny|allow`, default `deny`): a MAC learned on
  client A cannot be taken over by client B while A is **live**, i.e. A sent
  any datagram (frame or hello; with a key, a keyed hello) within
  `--live-window` seconds. B's frame is dropped, not forwarded
  (`dropped_macmove`; `--verbose` logs it). `allow` restores the old
  learn-anywhere behaviour. Pinning is only as good as the table behind it, so:
  - a MAC entry lives as long as its owner keeps talking (`--mac-timeout`
    counts from the newer of the entry's and the owner's last datagram), so a
    guest that is merely quiet does not lose its MAC while its keepalives go on;
  - a full MAC table only gives up an entry whose owner is gone, or one that
    belongs to the client that is learning, so source-MAC churn cannot push a
    live client's entries out; if nothing may go the frame is dropped
    (`dropped_macfull`);
  - a client holds at most `--max-macs-per-client` entries (default 8; the next
    one replaces its own least recently seen), so one client cannot fill the table;
  - **trunks** (the TAP port, the `--connect` uplink, and any client listed with
    `--trunk a.b.c.d[/n]` or `--trunk a.b.c.d:port`, i.e. a child hub that
    joined you) carry a whole LAN: they are exempt from the MAC cap, the rate
    limit and the flood cap, they may take a MAC from a guest (it moved to the
    other side), and their MACs always count as live, so a quiet host stack
    stays protected until `--mac-timeout`. A child hub that is *not* listed is
    just one more client and is limited like one; list it. A hub loop is
    unprotected by any of this, as before.
- **Rate limits**: per client a token bucket of `--rate FPS[,BPS]` (default
  10000 frames/s and 12.5 MB/s, i.e. 100 Mbit/s; burst a quarter of a second,
  at least 8 frames / one full frame); excess datagrams are dropped before
  anything else happens (`dropped_rate`). Broadcast amplification:
  `--flood-rate BPS` (default 2 MB/s) caps the egress bytes one client can
  cause by flooding (broadcast, multicast, unknown unicast; a frame costs
  length x copies, so one 1514-byte broadcast to 63 ports costs 95 KB), and the
  hub as a whole has a bucket 4x as big, which is charged only for senders that
  have already used more than half of their own budget, so a few heavy senders
  can empty it but never stop a quiet client's ARP; a flood over budget is
  dropped whole (`dropped_flood`). Trunks are not limited (see above). `--flood-rate 0` turns the flood cap off, `--no-rate`
  turns off both. Known unicast is never charged as a flood.
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
- `--key-file FILE` (also `--auth-window SEC`, default 60): see Authentication.
- Every option also takes the `--name=value` form.

### Why `--mac-move=deny` is the default

Legitimate MAC moves exist: a guest restarted on a new UDP port (the emulator
restarts, or a second emulator is started with the same `--eth-mac`). Denying
them for ever would be a bug; allowing everyone to move any MAC at any time is
the hijack. The compromise is *liveness*, not a timer on the entry: the owner
is protected while it keeps talking to the hub, including keepalive hellos (the
emulator sends one every 5 s), and released `--live-window` seconds (15)
after it goes quiet. So an attacker cannot take a MAC from a running guest
(and, because the spoofed frame is dropped rather than learned, cannot lock the
owner out either), and a restarted guest waits at most 15 s after its old
process stopped. What it does not cover: a client that sends no hello and
stays silent for 15 s (a QEMU endpoint, which never sends hellos) can have its
MAC taken over; use `--mac-move=allow` for those (QEMU cannot join a keyed
hub at all). A hub chain by `--connect` must remain a tree (as before), then
no MAC legitimately appears on two ports at once. One consequence: a guest
that restarts on the *other* side of a trunk is blocked until its old entry
ages out (`--mac-timeout`), because the trunk owner is always live.

### Authentication (`--key-file`)

Optional, off by default. With `--key-file FILE` (the file must not be
accessible by group or others, like an ssh key; 16..1024 bytes, trailing
newline ignored) the empty hello no longer registers a client. A client joins
with a **keyed hello**, a 13-byte datagram:

```
byte 0       0xE7
bytes 1..4   unix time, seconds, big endian
bytes 5..12  first 8 bytes of HMAC-SHA256(key, "ethhub-hello-v1" || bytes 0..4)
```

13 bytes is the range no Ethernet frame can have, so it is never mistaken for
a frame, and an old or unkeyed hub drops it as a short datagram. The hub
accepts a hello whose time is within `--auth-window` seconds of its own clock
(hosts need roughly synchronised clocks; the default is 60 s) and whose tag
matches. The sender's UDP address then becomes a client; frames are unchanged
(no per-frame header) and are accepted from known client addresses only.
Datagrams from any other address, including empty hellos and frames, are
dropped (`dropped_unauth`). **Only a keyed hello keeps a client alive**: frames
do not refresh it, and a client without a valid hello inside
`--client-timeout` expires. A hello must also carry a timestamp newer than the
last one accepted from that address (a repeat is `dropped_replay`), so a
captured hello cannot be used to stay in. The hub answers each accepted hello
with an empty datagram (keyed hubs only; an unkeyed hub never answers), which
the emulator and a `--connect` uplink use to tell that key and clocks are right.
The emulator sends a keyed hello every 5 s from a timer thread, so a paused
emulation (debugger, host sleep) keeps its place; if nothing has answered after
30 s it prints a one-time hint, and so does a hub whose uplink is silent
(`--hint-after`). `--connect` sends keyed
hellos when the hub has a key, so a keyed tree needs the same key everywhere.
The emulator side: `--eth=hub:HOST:PORT,key=FILE` or the environment variable
`MAXIVMAC_HUB_KEY_FILE` (the option wins), same file rules.

What this stops: a host on the LAN (or a port scanner) that does not have the
key cannot join, receive floods, send frames, take MACs, fill the client
table or use the hub as a reflector via spoofed sources that do not carry a
valid hello. What it does **not** stop, by design (this is a join ticket for
casual abuse, not a secure channel):

- *No confidentiality or integrity*: frames travel in clear and are not
  authenticated per datagram. Anyone who can read the traffic sees the LAN.
- *Replay*: a captured hello is valid for `--auth-window` (60 s) from any
  *other* source address (the tag does not cover the address because NAT and
  Docker hide the client's real one), and the client that replay creates
  expires `--client-timeout` after it (about two minutes in all) unless the
  attacker keeps replaying newer captured hellos, which needs sniffing the
  path all the time. A hello cannot be replayed from the address that sent it.
  Someone who can sniff the path can therefore join.
- *Source spoofing after joining*: while a client is registered (it expires
  without fresh hellos, which a spoofer cannot make), anyone who can forge that
  UDP source address can inject frames as it.
- *A shared key*: all clients have the same secret; there is no revocation
  other than changing it everywhere.
- *CPU*: every 13-byte datagram starting with 0xE7 costs one HMAC to check.
  That is a few microseconds; there is no separate limit on it.
- *UDP floods* that merely fill the socket or the network path.

Use `--allow` as well, and a VPN/SSH tunnel or a trusted LAN when any of that
matters. The key is never passed on a command line (it is a file for that
reason). The hub and the emulator open it without blocking, require a regular
file owned by the running user with no group/other permission bits, warn about
a key that is one repeated byte, and wipe their copies of the secret (the HMAC
pads are derived once at start and the key buffer is zeroed at once). **QEMU
cannot join a keyed hub at all**: it cannot send the keyed hello, and an
`--allow` rule does not help because the keyed hub drops everything from an
address that has not sent one. Put a QEMU endpoint on an unkeyed hub (with
`--allow`) and connect that hub to the keyed one with `--connect`/`--trunk`.

### Security

Without `--key-file` there is no authentication. Any datagram from any source
that reaches the socket makes that source a client, and every broadcast is
flooded to all clients. A hub that listens beyond loopback can therefore be
used for UDP reflection and amplification (spoofed source addresses) and lets
anyone who can reach it read the LAN's broadcast traffic. Hence the loopback
default, the warning, `--allow`, and the hardening above: MAC pinning, rate
and flood caps, bounded tables that cannot be locked by a hello flood, and
the optional key. Do not expose a hub to the internet: nothing here encrypts
anything, and a rate limit bounds an abuser, it does not stop one.

Single-threaded `poll()` loop; plain C11 + POSIX, no dependencies. Runs on
macOS and Linux.

## Build and test

- CMake: `-DMAXIVMAC_ETHHUB=ON` (default on UNIX) adds targets `ethhub` and
  `ethhub-asan`; `ctest -R ethhub` runs `tools/ethhub/test/test_ethhub.py`
  against the ASan/UBSan binary (about 45 s; includes registration, flooding,
  unicast, ageing, size limits, table bounds and LRU, MAC pinning, rate and
  flood caps, client eviction, the per-IP cap, authentication, pcap, uplink,
  the loopback default and `--allow`, a 6000-datagram random fuzz).
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

PeerTalk's `docker/Dockerfile.posix` is a plain
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

- **Abuse limits** (measured against the hub on loopback with scripts before
  the hardening; each is now covered by a test in `test_ethhub.py`):
  - *Client table exhaustion* (64 hellos from 64 source ports filled the table
    until `--client-timeout`): idle clients are evicted, one IP is capped at
    `--max-per-ip`. Still possible: a flood of hellos from many *source IPs*
    (spoofed) that keeps the table full of fresh entries; only a key or
    `--allow` stops that, and clients that keep their keepalive going are
    never evicted by it.
  - *Amplification* (one 1514-byte broadcast = 63 copies, about 95 KB): capped
    per client and for the hub by `--flood-rate`. A reachable hub is still a
    reflector for traffic up to the cap unless it has a key or `--allow`.
  - *MAC hijack*: `--mac-move=deny`, see above.
  - *Rate*: `--rate`. The defaults are generous (100 Mbit/s per client), so
    they bound an abuser to a LAN-sized stream, they do not protect a slow link.
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
  meaningful macOS number was taken. Lookups are O(1) hash lookups now (they
  were linear scans); the latency figures above are from the linear version
  and were not re-measured.

## QEMU compatibility (NOT verified)

QEMU's UDP/dgram network backends (`-netdev dgram,...` and the older
`-netdev socket,udp=...`/`-net socket,udp=`) are documented to carry one raw
Ethernet frame per datagram with no extra header, which is what ethhub uses,
and QEMU would join by sending to the hub's address. This has **not** been
tested here: no QEMU was run against ethhub. In particular the hello/keepalive
(zero-length datagram) is an ethhub convention that QEMU does not emit, so a
QEMU endpoint only becomes known to the hub when it first sends a frame (and a
keyed hub never admits it, see Authentication), and
it would also age out after the client timeout unless it keeps sending; use a
long `--client-timeout` or a small helper that sends keepalives.
