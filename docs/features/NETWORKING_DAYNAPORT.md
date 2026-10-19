# DaynaPORT Ethernet (SCSI/Link)

maxivmac can present a DaynaPORT SCSI/Link Ethernet adapter on the SCSI bus.
Unlike the SLIP serial route ([NETWORKING.md](NETWORKING.md)) the guest sees a
real Ethernet link: EtherTalk, and MacTCP over Ethernet, with the stock Dayna
driver. It needs a Mac with SCSI (Plus, SE, Classic). The Mac II family is not
supported: `--scsiN` there is refused with an error, because /DACK pseudo-DMA
is not decoded on that path and a driver could not transfer data.

```sh
maxivmac --model=MacPlus --rom=MacPlus.ROM --scsi4=daynaport --eth=slirp disk.hfs
maxivmac --model=MacPlus --scsi4=daynaport \
         --eth=slirp,hostfwd=tcp::8080-10.0.2.15:80 --eth-mac=02:00:00:aa:bb:cc disk.hfs
```

## Options

| Option | Meaning |
|--------|---------|
| `--scsiN=daynaport[,multiframe]` / `none` | Attach (or not) a DaynaPORT at SCSI ID N, 0-6. Any ID works; the Mac is ID 7. `multiframe` packs several received frames into one READ (default: one frame per READ, like BlueSCSI; unverified against the real driver). |
| `--eth=slirp[,hostfwd=...]` | Backend. `slirp` is user-mode NAT; `hub:HOST:PORT` joins an ethhub virtual LAN (below); `none` is a cable that is out. **Default:** `slirp` in an interactive run (when built with libslirp), `none` with `--headless` or `--dbg-script`. libslirp runs on the host clock and real sockets, so a live link would make scripted runs non-deterministic; give `--eth=slirp` explicitly to get one there (a note is printed when the default applies). |
| `hostfwd=tcp:[HOSTIP]:HOSTPORT-GUESTIP:GUESTPORT` | Host-to-guest forward, TCP or `udp`; `GUESTIP` may be empty (= 10.0.2.15). `HOSTIP` is the host address to listen on, a dotted quad; **empty means 127.0.0.1**, so the forward is reachable from this machine only. `0.0.0.0` (or an interface address) exposes it to the network and prints a warning. Repeatable. |
| `--eth-mac=xx:xx:xx:xx:xx:xx` | Adapter MAC. Default `02:6d:78:76:6d:01` (locally administered); with `--eth=hub:` the default is a random `02:6d:78:xx:xx:xx`, printed at start. A guest can override it with the driver's set-MAC command. |
| `--eth-latency=DURATION` | Minimum delay before a received frame is visible to the guest, measured on the **emulated** clock. Default `10ms`; `500us`, `1s` (at most one second); `0` or `off` disables. A unit is required. See "Receive latency" below. |
| `--net-redir=tcp:HOST:GUESTIP:GUEST` | Older form of host forward (loopback only, no host address). Applies to `--eth=slirp` and to `--serial-a=slip`. `--slip-redir` is kept as an alias. |
| `--diag=scsi` | Trace every CDB, data record and status the target sees. |

The same settings exist as `.mac` file keys: `scsi0` ... `scsi6`, `eth`,
`eth-mac`, `eth-latency`, and (repeatable) `net-redir`.

slirp addressing (one `SlirpStack::Config` in the code): network 10.0.2.0/24,
gateway 10.0.2.2, DNS 10.0.2.3, guest 10.0.2.15. Configure MacTCP manually with
those, or use the gateway as router; there is no DHCP-driven setup in System 6
MacTCP.

## Architecture

```
guest driver --SCSI--> Ncr5380 --ScsiTarget--> DaynaPortTarget --EthernetBackend--> EthSlirp --> SlirpStack --> libslirp
                                                    |                              EthNull ("none")
                                                    +-- MAC, multicast list, statistics
```

`EthernetBackend` (`src/devices/eth_backend.h`) is the seam: `linkUp`,
`send` (14..1514 bytes, no FCS), `peekSize`/`receive` (peek before dequeue),
`poll` (once per emulated tick, non-blocking) and a drop counter. Backends run
on the emulation thread and never block; start-up may be asynchronous
(`linkUp` false then true). Each backend owns a 64-frame receive queue that
drops the oldest frame on overflow (`FrameQueue`). `CreateEthBackend()`
(`eth_factory.cpp`) is table driven, so another backend (a hub, a tap) is one
table entry. Tests use `FakeEthernet` (`test/eth_fake.h`) only.

`DaynaPortTarget` owns the adapter state: MAC, multicast list, receive filter,
statistics, enable flag. Presence means "attached"; there is no probe through
the backend.

## What the target does differently from the donor

The command set comes from the Mini vMac DaynaPORT fork by Mikhail Kandel
(GPL-2.0-only), whose protocol references are BlueSCSI-v2 and the PiSCSI Dayna Port notes
(protocol facts only; no code from either is copied here).
Fixes, each with a test in `test/test_daynaport.cpp`:

1. READ peeks the next frame's size before dequeueing. A frame that cannot
   fit the READ's allocation length is **dropped** (counted in `rxOversize`)
   and the READ returns the empty six-byte header. Leaving it queued behind
   CHECK CONDITION would wedge receive: the real driver never sends REQUEST
   SENSE and retries with the same length.
2. A record is `[size:2][flags:4][frame+FCS:n+4]`; flag byte 5 = 0x10 on a
   record when another frame is still queued. With the `multiframe` option
   READ packs several records while they fit (the frame after the last one
   that fits stays queued); by default it returns one. A lone frame is
   byte-identical to the donor's layout. The 0x10 value is unverified
   (PiSCSI's `e_more_data_available` may be 0x00000001).
3. Link down is not a SCSI error: TEST UNIT READY succeeds, READ returns an
   empty record, WRITE is dropped silently.
4. Works at any SCSI ID.
5. Statistics are real: GET MAC returns the MAC plus three big-endian
   counters (alignment errors, CRC errors, frames lost = backend queue drops).
   rx/tx/tx-dropped/filtered/oversize counts are kept in the target
   (saturating). Frames are dequeued when a READ is built, so a READ cut short
   by a bus reset or ABORT loses them; they are counted in `rxLost`.
6. MAC override (`0x0C` with flag `0x40`) is accepted; `0x0D` replaces the
   multicast list; receive filtering passes our MAC, broadcast and listed
   multicast only.
7. Bus reset and BUS DEVICE RESET restore the power-on state (disabled,
   default MAC, empty multicast list, queue and sense cleared). ABORT does not.
   Sense data lasts until the next command other than INQUIRY and REQUEST SENSE.

The NCR5380 bus gained `ScsiTarget::cdbLength()` so the Dayna vendor opcode
`0x80` is a 6-byte CDB rather than the group-4 length of 16.

## What the real driver does

Observed with the System 6.0.8 image with the original Dayna SCSI/Link driver
(Network control panel, "EtherTalk Alternative"), `--scsi4=daynaport --diag=scsi`:

```
reset                                       (SCSI bus reset by the SCSI Manager, twice at boot)
08 00 00 00 01 00   READ, 1 byte   -> CHECK CONDITION   (boot-time probe, twice; no INQUIRY at boot)
-- user chooses EtherTalk in the Network cdev --
12 00 00 00 25 00   INQUIRY 37     -> "Dayna   SCSI/Link       2.0f"
0E 00 00 00 00 80   ENABLE
09 00 00 00 12 00   GET MAC (18 bytes)
0D 00 00 00 06 00   SET MULTICAST, 6 bytes = 09:00:07:FF:FF:FF (AppleTalk)
09 00 00 00 12 00   GET MAC
0A 00 00 00 32 80   WRITE, framed: 4-byte header (00 32 00 00), 50-byte frame, 4-byte zero header
08 00 00 05 F4 C0   READ 1524, polled ~every 10 ms between WRITEs; empty record (6 bytes)
```

The 50-byte frames are AppleTalk AARP probes (802.3/SNAP, `AA AA 03 00 00 00 80 F3`)
to `09:00:07:FF:FF:FF`, followed by 42- and 69-byte frames (NBP). The switch to
EtherTalk completes without error, with `--eth=slirp` and with `--eth=none`.
The driver never issued REQUEST SENSE, MODE SENSE or the vendor 0x80 command in
this session. Not exercised: receiving a real frame, multi-frame READ and MacTCP.

## Timing and the wall clock

REQ is asserted with zero latency; nothing is paced by the wall clock, and the
driver worked without delays. libslirp, however, runs on the host's steady
clock (TCP retransmit and ARP timers) while `poll()` runs once per emulated
tick. At high `--speed` the guest runs many times faster than real time, so
network round trips look very slow to it, MacTCP may time out and
retransmit in bursts ("retransmit storms"), and a stalled host leaves the guest
believing the network is gone. On the Mac SE, AppleTalk start-up needs `--speed=0` (real-time) or it bombs with
"divide by zero" (FINDINGS F41). Keep the speed at 1x for networking until this
is measured with MacTCP. libslirp's timers are fired from `poll()` (their
expiry is in milliseconds on the clock `clock_get_ns()` reports in nanoseconds).

## Joining an ethhub: `--eth=hub:HOST:PORT`

`EthHub` (`src/devices/eth_hub.{h,cpp}`) speaks the wire format of
`tools/ethhub` (`ETHHUB.md`): one raw Ethernet frame (14..1514 bytes) per UDP
datagram, a zero-length datagram as hello/keepalive. Start a hub, then point
each emulator at it:

```sh
ethhub --listen 127.0.0.1:5555 &
maxivmac --scsi4=daynaport --eth=hub:127.0.0.1:5555 disk1.hfs
maxivmac --scsi4=daynaport --eth=hub:127.0.0.1:5555 disk2.hfs     # another instance, same LAN
```

- The socket is non-blocking; the emulation thread never waits. A frame is
  dropped (and counted) when the socket would block. `HOST` (IPv4 address or
  name) is resolved once, at start, so a name lookup can delay start-up but
  nothing later. IPv6 literals are not supported (the hub is IPv4 only).
- A hello goes out at start and then every 5 s of **wall** time (the hub drops
  a client after 60 s of silence), however fast or slow the emulation runs, and while it is paused (a timer
  thread sends them, not the emulation tick).
- **Key.** `--eth=hub:HOST:PORT,key=FILE` (or `MAXIVMAC_HUB_KEY_FILE=FILE`)
  makes the hello the 13-byte keyed hello that a hub started with
  `--key-file` requires (`ETHHUB.md`, Authentication; `eth_hub_auth.h`). FILE
  must be mode 0600, 16..1024 bytes; a bad file means "no network" with a
  message, never a silent plain hello. Without a key the wire is unchanged. A
  keyed emulator against a hub without a key does not join (the hub drops the
  hello as a short datagram); the clocks of the two hosts must agree to within
  the hub's `--auth-window` (60 s).
- Received frames go through the usual 64-frame drop-oldest queue. Datagrams
  from any source other than the hub, and datagrams of 1..13 or more than 1514
  bytes, are ignored and counted. Counters are printed to stderr when the
  emulator exits if anything was dropped.
- **Link state is not hub liveness.** UDP has no connection, so the link is
  "up" as soon as the socket exists, hub or no hub. With no hub listening the
  guest sees a link that carries nothing.
- **MACs.** Two emulators with the same MAC on one hub would silently steal
  each other's traffic (the hub learns MAC -> client). With a hub and no
  `--eth-mac`, the adapter therefore gets a random locally administered MAC
  `02:6d:78:xx:xx:xx` (24 random bits, negligible collision odds for a handful
  of emulators) and prints it. Pass `--eth-mac` for a fixed, repeatable MAC
  (needed for repeatable scripted runs). The guest can still override it with
  the driver's set-MAC command; that is the guest's responsibility.
- Tests: `test/test_eth_hub.cpp` (loopback fake hub, fake wall clock, the
  RFC 4231 HMAC vectors, key files) and the
  ctest `eth_hub_ethhub`, which (plain, then with a key) starts the real `ethhub` and runs three
  `EthHub` backends through broadcast, unicast and unknown-unicast flooding
  (`test/eth_hub_it.cpp`, driver `test/eth_hub_it.py`; needs `MAXIVMAC_ETHHUB`).
- `--eth-latency` applies to hub frames too (default 10 ms of emulated time).
  Frames are collected once per emulated tick, so a hub frame reaches the
  guest up to one tick (1/60 s) after it arrives at the socket.

## Receive latency

libslirp answers an ICMP echo or an ARP request inside `send()`, so the reply
was already queued before the guest's WRITE had returned, and the very next
READ (2 to 4 ms of emulated time later) delivered it. MacTCP Ping loses a reply that arrives that early: echo #2
and later were logged as timeouts although every reply reached the guest
(observed with System 6.0.8, MacTCP 2.0.6, MacTCP Ping 2.0.2; the first echo
survives only because it goes out from MacTCP's deferred queue after the ARP
exchange). A real LAN never answers within microseconds either.

`EthLatency` (`src/devices/eth_latency.h`) is a decorator around any
`EthernetBackend`. It time-stamps each received frame when it first sees it
(right after every `send()`, and in `poll()`) and shows it to the target only
`--eth-latency` later. The clock is the emulator's cycle counter
(`g_ict.getCurrent()`), so the delay is guest time: deterministic for a given
emulated run, independent of `--speed`, of the host clock and of how often the
driver polls. Frame order is kept and the held queue is bounded like every
backend's queue (64 frames, drop-oldest, counted in the lost-frames statistic).
Backends stay simple (they know nothing about it); the factory
(`CreateScsiTarget`) adds the decorator when a clock is supplied and the
latency is not zero. Unit tests: `test/test_eth_latency.cpp` (fake clock,
`FakeEthernet`, and a DaynaPORT READ after a WRITE).

**How the default was chosen.** The first guess, 2 ms, did not help: the
guest's next READ already comes 2.2 to 4.2 ms after the WRITE (measured on the
emulated clock), so a 2 ms hold changed nothing. With the full MacTCP flow of
`test/net/run-ping.sh` (5 echoes to 10.0.2.2, scored 5 out / 5 in):

| `--eth-latency` | 0 / 2 ms | 4 ms | 6 ms | 8 ms | 10 ms | 14 ms | 20 ms | 100 ms | 300 ms |
|---|---|---|---|---|---|---|---|---|---|
| result | 1 of 5 | fail (6 requests) | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |

The reply has to arrive more than about 4 ms after the send; 10 ms leaves a
margin and is still far below any timeout MacTCP uses. Why MacTCP Ping needs
that long was not investigated in the guest.

A latency also adds to real round-trip times (a ping through slirp shows
about 10 ms more than the host path), which is harmless for the 1 s resolution
of the classic TCP/IP tools.

## Limits

- No tap/bridge backend (the factory leaves room for it).
- Only unicast/broadcast/exact-match multicast filtering; no hash filter, no
  promiscuous mode.
- Receive mode bits of `0x0C` other than the MAC override are accepted and ignored.
- The multi-frame flag value (0x10 in the last flag byte) follows the
  BlueSCSI/PiSCSI protocol notes and has not been checked against the Dayna
  driver with more than one frame pending; hence packing is opt-in.
- Two DaynaPORTs (`--scsi3=daynaport --scsi4=daynaport`) each get their own
  slirp instance with the same addressing, MAC and host forwards: the second
  adapter's forwards fail to bind (a warning is printed) and the two guests
  would share the 10.0.2.15 address. Use `--eth=none` on one of them.
