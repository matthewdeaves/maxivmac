# Containerised end-to-end test: System 6 guest, ethhub, PeerTalk

`test/e2e/run.sh` runs a System 6.0.8 guest in headless maxivmac on a virtual
LAN (`ethhub`) together with a POSIX PeerTalk peer, **all inside one
container**, and checks that the two talk: ICMP, PeerTalk UDP-broadcast
discovery, exactly one TCP connection, messages both ways. It exists to test
what neither the emulator's unit tests nor `test/net` (slirp, no TCP, no second
host) can: a real 68k MacTCP program against a real little-endian Linux stack
over a shared layer-2 segment.

Relevant background: [ETHHUB.md](ETHHUB.md) (the hub, `--tap`),
[NETWORKING_DAYNAPORT.md](NETWORKING_DAYNAPORT.md) (the adapter, `--eth=hub:`),
[NETWORKING_TESTING.md](NETWORKING_TESTING.md) (the guest disk and the scripted
MacTCP setup).

```sh
export MAXIVMAC_ROM=extras/roms/vMac.ROM            # Mac Plus ROM (not in the image)
export E2E_DISK=$PWD/e2e-guest.hfs                  # from test/e2e/make-disk.sh
export PEERTALK_DIR=~/peertalk CLOG_DIR=~/clog      # read-only inputs
export BOMBERTALK_DIR=~/bombertalk                  # optional, R4
test/e2e/run.sh r1 r2 r3                            # default; also r2b, r3b, r4
```

## What runs where (native vs container)

| Where | What |
|---|---|
| **Native (host)** | `test/e2e/run.sh`: stages the build context, `docker build`, `docker run`, collects `/out`, host-side PT_Log read-back with hfsutils. It is native because it **drives Docker**. `test/e2e/make-disk.sh` (and `test/net/prep-disk.sh`, `build-peertalk-68k.sh`, `add-apps.sh` it calls) are native too: they need macOS `xattr`, hfsutils, unar and the Retro68 cross compiler, none of which is in the image. |
| **In the container** | everything that runs during a test: `ethhub`, the headless emulator, the POSIX PeerTalk/BomberTalk peer, the UDP log receiver, `ping`, the pcap analysis, the checks (`test/e2e/in-container/e2e.py`, python3 stdlib only). |

No ROM, disk image or Apple/Dayna/third-party software is in the image or in
the repository. They are read-only mounts at run time: `/in/rom/vMac.ROM`,
`/in/disk/guest.hfs`. The image holds only what is built from source
(maxivmac, ethhub, PeerTalk test programs, BomberTalk's SDL build) plus the
project's own `data/debug/` (trap names for `wait text`).

## The image (`docker/e2e/Dockerfile`)

`maxivmac-e2e:<VERSION>-<hash of the staged build context>` (built when absent,
so a changed source gets a new tag), about 130 MB.

- Base `debian:trixie-slim`, pinned by digest. **Why Debian 13:** maxivmac
  needs SDL3 (always, even for `--headless`) and trixie is the oldest
  Debian/Ubuntu that packages it (`libsdl3-dev` 3.2.x); Debian 12 and Ubuntu
  24.04 do not, so SDL3 would otherwise be built from source. glibc, not musl,
  because SDL3 and Mesa's GL stubs expect it (the Alpine `ethhub-peer` image
  stays the way to run a hub alone).
- Stage 1: maxivmac (cmake, Release) and ethhub. **libslirp is not
  installed**: the build auto-detects it, the test only uses `--eth=hub:`, and
  trixie's libslirp 4.8 is older than the 4.9 API `slirp_stack.cpp` needs.
- Stage 2: PeerTalk's POSIX test programs (`test_mesh`, `test_chat`,
  `test_lifecycle`, `test_multi`) from `${PEERTALK_DIR}` with clog from
  `${CLOG_DIR}` (nothing is fetched: `FETCHCONTENT_FULLY_DISCONNECTED`); a second
  copy from `${PEERTALK_DIR_OLD}` as `/opt/peers/old` when given (the pre-fix
  control). Stage 2b: BomberTalk's SDL2 build (`tools/build-sdl.sh`) and
  `sdlkeys.so`.
- Runtime: python3, iproute2, iputils-ping, SDL3/SDL2/GL runtime libraries.
  The context is a staged, filtered copy (`run.sh` rsyncs the repo without
  `bld/`, docs, media, `test/net`, `test/e2e`, ...) plus `.dockerignore`, so
  editing a test script does not rebuild the emulator layer.

## Container conventions and the NET_ADMIN exception

`run.sh` starts **one one-shot container per rung** (`docker run --rm`, no
compose labels):

```
docker run --rm --name maxivmac-e2e-<rung>-<pid>-<n> \
  --network none \
  --cap-drop ALL --cap-add NET_ADMIN --cap-add SETUID --cap-add SETGID \
  --device /dev/net/tun \
  --security-opt no-new-privileges \
  --read-only \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m \
  --tmpfs /work:rw,nosuid,size=768m,uid=2010,gid=2010,mode=0700 \
  --cpus 2 --memory 1g --memory-swap 1g --pids-limit 256 \
  --log-driver json-file --log-opt max-size=10m --log-opt max-file=3 \
  -v <staged inputs>:/in:ro -v <output dir>:/out \
  -e E2E_FLAT=1 -e E2E_TIMEOUT=1500 -e E2E_PEER=new -e E2E_SPEED= \
  maxivmac-e2e:<tag> <rung>
```

plus a host-side watchdog (`docker kill` after the timeout + 120 s), a
`docker stats` sample every 20 s, and the output cap (the driver copies at most
64 MB to `/out`; each child process has `RLIMIT_FSIZE` 96 MB).

**Network:** `--network none`. The only interfaces are `lo` and `tap0`. `tap0`
is created by the entrypoint and is one port of `ethhub`; the other port is the
emulator. The hub listens on `127.0.0.1:5555` inside the container. Nothing
leaves the container.

**Capabilities, the documented exceptions.** Creating a tap device needs
`CAP_NET_ADMIN` in the process that does it, and a non-root uid cannot hold a
capability that the runtime did not give it as an ambient one (Docker does not),
so the entrypoint starts as root:

1. as root, with `NET_ADMIN` it runs `ip tuntap add dev tap0 mode tap user
   2010`, sets MAC and address, brings it up and adds two host routes (below).
   **This is the only privileged step**, and the capability is confined to the
   container's own, otherwise empty, network namespace (`--network none`), so
   it cannot touch the host or any other container;
2. it `exec`s `setpriv --reuid=2010 --regid=2010 --clear-groups --no-new-privs`
   into the driver. Leaving uid 0 empties the permitted and effective sets, and
   no_new_privs stops `execve` regaining any. No root process remains.

Deviation from "everything dropped except NET_ADMIN": **`SETUID` and `SETGID`
are also added**, only so that step 2's `setresuid` is allowed; `setpriv
--bounding-set=-all` would need a fourth capability (`SETPCAP`) and was left out,
so the bounding set still lists the three (`CapBnd 00000000000010c0`) while the
permitted/effective sets are empty. The driver asserts this at the start of
every run (`unprivileged` check: uid 2010, `CapPrm`/`CapEff` 0, `NoNewPrivs` 1)
and records it in `summary.json`. Everything that matters (ethhub, the
emulator, the peers, ping) runs as uid 2010 with no capability. `ping` works
unprivileged through ICMP datagram sockets.

**Two routes.** With `--network none` there is no default route, which broke
PeerTalk's POSIX backend in two ways found while writing R2: it learns its own
address by `connect()`ing a UDP socket to 8.8.8.8 (nothing is sent) and, with
no route, falls back to `127.0.0.1`; and it broadcasts discovery to
`255.255.255.255`, which Linux refuses (`ENETUNREACH`) without a matching route,
so the guest never heard it. The entrypoint adds `8.8.8.8/32 dev tap0` and
`255.255.255.255/32 dev tap0`. Both are on the closed virtual LAN, nothing else
is routed.

## Inputs

| Variable | Meaning |
|---|---|
| `MAXIVMAC_ROM` | Mac Plus ROM (default `data/roms/MacPlus.ROM`, else `extras/roms/vMac.ROM`) |
| `E2E_DISK` | the guest disk (below) |
| `PEERTALK_DIR`, `CLOG_DIR` | PeerTalk and clog sources, **read-only** (nothing in them is modified) |
| `BOMBERTALK_DIR` | optional, BomberTalk sources (R4) |
| `PEERTALK_DIR_OLD` | optional second PeerTalk (e.g. an older commit via `git archive`), built as `/opt/peers/old`; use with `E2E_PEER=old` |
| `E2E_OUT`, `E2E_IMAGE`, `E2E_REPEAT`, `E2E_CPUS`, `E2E_MEMORY`, `E2E_PIDS`, `E2E_TIMEOUT`, `E2E_EXPECT_FAIL` | see the header of `run.sh` |
| `E2E_PEER`, `E2E_SPEED` | `old` selects the pre-fix POSIX peer; `E2E_SPEED` is the emulator's `--speed` |

If an input is missing `run.sh` prints why and exits **77** (skip). Exit 0: all
rungs passed, 1: a rung failed.

Colima / Docker Desktop only bind-mount some host directories (the home
directory), so `run.sh` stages the inputs under the output directory and not in
`$TMPDIR`: a bind mount of a path the VM cannot see silently becomes an empty
directory.

### The guest disk (`test/e2e/make-disk.sh`)

`test/net/prep-disk.sh` (System 6.0.8 + DaynaPORT + AppleTalk + MacTCP + MacTCP
Ping, see NETWORKING_TESTING.md) plus, new here:

- `test/net/build-peertalk-68k.sh OUT test_mesh test_chat`: PeerTalk's 68k
  MacTCP apps, built out of tree with Retro68 from `PEERTALK_DIR`/`CLOG_DIR`;
- `test/net/add-apps.sh BASE OUT app.bin ...`: both forks into `:Net Tools:` with
  hfsutils (MacBinary);
- the LAN address is not on the disk: **MacTCP is configured in-session**, as
  before, by `test/net/mactcp-config.py ADDRESS/PREFIX [--gateway ...]`, which
  emits the dialog gestures (class A boxes Net / Subnet / Node; identical to
  `mactcp-config.inc` for 10.0.2.15/24; `/8` leaves the slider alone; no gateway
  by default). The EtherTalk switch lives in PRAM, which is not kept between
  runs, so every run repeats it in-session (about 50 s of emulator time).

The Finder lays the four applications out in name order (BomberTalk, MacTCP
Ping, test_chat, test_mesh); `e2e.py` clicks those positions (`ICON_X`).

## The ladder

Every rung is a script (assembled from `test/net/scripts/ping/*.inc` plus the
generated MacTCP block and a rung-specific tail), a set of assertions, and
evidence in `<out>/<rung>/`: `summary.json` (per-check PASS/FAIL, timings,
container cgroup usage), `lan.pcap` (every frame the hub received),
`hub.out` (ethhub's SIGUSR1/exit counters), `capture.log` (the PeerTalk debug
broadcast received over the hub), `peer.log`, `emulator.out`, `run.dbg`,
`shots/*.png`, `disk-after.hfs.gz`, `container.log`.

| Rung | Guest (MacTCP, manual) | Container (tap0) | What is asserted |
|---|---|---|---|
| R1 | 10.88.0.20/24, no gateway | 10.88.0.10/24 | MacTCP Ping to 10.88.0.10 shows `5/5/0/0` and the pcap holds 5 echo requests and 5 replies from the container's kernel; then the container's `ping 10.88.0.20` receives 5/5 and the pcap shows 5 requests and 5 guest replies |
| R2 | 10.88.0.20/24 | 10.88.0.10/24, `test_mesh` | the POSIX peer discovers the guest by UDP broadcast, the guest has seen the peer, exactly one TCP connection forms, dialed by the bytewise-lower address, both sides report `[CONN]`, heartbeats flow both ways, the guest's own verdict is `*** PASS ***` |
| R2b | 10.88.0.20/24 | 10.88.0.30/24, `test_mesh` | as R2 but the **guest** is the designated initiator, so the guest can only dial after it discovered the peer by broadcast: it must log `[DISC] POSIX @ 10.88.0.30` |
| R3 | 10.88.0.20/**8** | 10.0.2.25/**8** | the cross-endianness case (below): same checks, one connection, dialed by 10.0.2.25 |
| R3b | 10.88.0.20/8 | 10.0.2.15/8 | the pair as first specified (below): ranks agree under both readings |
| R4 | 10.88.0.20/24, BomberTalk 68k | 10.88.0.10/24, BomberTalk SDL build | each side logs `Lobby entered`, `Peer discovered: .. (<other ip>)` and `Connected to:`, one TCP connection |

`test_mesh` (PeerTalk `tests/test_mesh.c`) is the program: it dials a
discovered peer only if `PT_ShouldInitiate` says so, broadcasts a heartbeat
every 3 s over `PT_RELIABLE`, counts the other side's heartbeats, and prints
a verdict. Its output reaches the harness through PeerTalk's own
**UDP debug broadcast** (`test_remote_log_enable`, port 7356, tagged
`[name@ip]`, see PeerTalk's CLAUDE.md "Remote Test-Log Capture"): the driver binds a UDP
socket in the container and receives both the guest's and the POSIX peer's
lines over the hub. The peer's stdout is a second source. `PT_Log` on the guest's disk is
read back after the run with hfsutils on the host as a cross-check
(`ptlog_crosscheck` in `summary.json`; recorded under `not_verified` when the
host has no hfsutils).

### Why R3 uses 10.0.2.25, not 10.0.2.15

PeerTalk's byte-order bug (253e821): addresses were compared as native
integers, so a little-endian peer ranks `10.0.2.15` against `10.88.0.20` by the
**last** octet (15 < 20: peer lower) and a big-endian Mac by the **first**
differing octet (second octet 0 < 88: peer lower). Those agree, so that pair
cannot tell fixed from unfixed code. `10.0.2.25` can: little-endian says the
guest is lower (20 < 25), big-endian says the peer is lower (0 < 88), so before
the fix each side thought the other should dial and nobody did. The driver
records both readings in `summary.json` (`facts.native_int_rank`). R3b keeps
the original pair as a control.

## Results recorded with this change

Colima (Docker 29.5.2, aarch64 Linux VM, 6 CPUs), image `maxivmac-e2e:1-f9dc0ffd`
(130 MB), PeerTalk `253e821`, `--cpus 2 --memory 1g`. Every rung was run three
times, up to three containers at once on the same VM. Wall time is the whole
container (the emulator is single-threaded and uses one core throughout, the
cgroup `cpu_seconds` is about the wall time).

| Rung | Runs | Wall | Memory peak | Observed |
|---|---|---|---|---|
| R1 | 3/3 PASS | 50 s | 30 MB | guest ping 5/5 (5 requests, 5 replies in the pcap), container ping 5/5 (rtt 5 to 22 ms) |
| R2 | 3/3 PASS | 126 s | 30 MB | peer dials (10.88.0.10 < .20); 1 connection; heartbeats guest received 27-28, peer 29 |
| R2b | 3/3 PASS | 126 s | 30 MB | guest logs `[DISC] POSIX @ 10.88.0.30`, dials; heartbeats 19 / 21 |
| R3 | 3/3 PASS | 126 s | 30 MB | 10.0.2.25 dials 10.88.0.20; 1 connection; heartbeats 28 / 29 |
| R3b | 3/3 PASS | 126 s | 30 MB | same, 10.0.2.15 |
| R4 | 3/3 PASS | 205 s | 38 MB | both sides `Lobby entered`, `Peer discovered`, `Connected to:`; the Mac lobby screenshot lists `BomberTalk (10.88.0.10)` |

**The pre-fix control.** The same R3 with the POSIX peer built from PeerTalk
`63e7201` (the commit before the byte-order fix; `git archive`, the user's
checkout is untouched), `E2E_PEER=old E2E_EXPECT_FAIL=1`: **3/3 fail the way the bug
predicts** (XFAIL): both sides log `[DISC]` of the other (UDP discovery works),
**no SYN is ever sent**, no heartbeat flows, both end `INCONCLUSIVE: no peers
connected`. The controls that agree under both readings pass with the old peer
(R2 and R3b, 1 run each), so it is the address order, not the old binary,
that stops R3. What this proves and does not: the 68k guest was *not* rebuilt
against the old library, because it does not need to be: on a big-endian CPU the
native-integer comparison equals the byte-wise one, so the guest behaves the
same before and after the fix; only the little-endian POSIX side changes
(not verified by building an old 68k binary).

**Flakiness.** 18 container runs of the final code (six rungs, three each) and
5 control runs gave no flaky result. Two flakes were found and fixed on the
way, both in the test and not in the system under test: the container's ping at
the Finder (MacTCP not yet opened, see above) and a second, unanswered SYN
after the guest quit (test_mesh redials) breaking an "exactly one SYN" check,
which now counts answered connections.

Evidence stays outside the repository (`e2e-out/` is git-ignored): on the
author's machine `/Users/matt/Documents/worktrees/maxivmac-e2e/e2e-out/final2/`
(runs `a`: R1 R2b, `b`: R2 R4, `c`: R3 R3b, `<label>/<rung>-<n>/`) and
`.../e2e-out/old/` (the controls). Each holds `summary.json`, `lan.pcap`,
`hub.out`, `capture.log`, `PT_test_mesh.txt` (the guest's PT_Log, read back from
the disk with hfsutils), screenshots.

## Notes that cost time

- **Guest speed.** The emulator's default is `--speed=4` (16x CPU per guest
  tick) and script times (`wait for 8s`) count emulated CPU time, not the guest's
  clock: at `--speed=0` the guest is too slow for the scripts' fixed waits (the
  `ether` step breaks), so the default stays and a script second is about 1/16
  of a guest second. The guest's own clock runs at about wall-clock rate, or
  slower when the host is busy (several containers sharing a Colima VM made
  `test_mesh`'s `[10s]` lines 20 to 30 s apart); PeerTalk's timers on the guest
  are guest-clock timers, those on the POSIX peer are wall-clock, and the runs
  below still agree.
- **MacTCP answers ARP and ICMP only after an application has opened it.** At the
  Finder, right after the second restart, `ping` from the container gets "Host
  Unreachable"; once MacTCP Ping (or any MacTCP program) is open it answers.
  R1 therefore pings the guest after the guest's own ping run.
- `key return` is down for 0.16 ms, which BomberTalk (it polls `GetKeys`) never
  sees. `key <spec> hold <duration>` was added to the debugger for it.
- BomberTalk's SDL build only reads `SDL_GetKeyboardState` and starts PeerTalk
  discovery in the lobby, so under the dummy video driver nothing could leave the
  menu; `sdlkeys.c` (LD_PRELOAD) holds Return once. Its `BT_SCREENSHOT` hook
  quits after the menu and is not used. The POSIX lobby cannot be looked at (dummy
  video); the log lines stand in.
- Docker's legacy builder (no buildx) is what Colima has here, so the build
  context is assembled by `run.sh` and there are no `--build-context`s.

## Not covered / not verified

- The guest's **PT_Log read-back** and the host-side `ping`/DNS beyond the LAN.
- Real-time (`--speed=0`) behaviour of the guest (the scripts cannot run there).
- Only System 6.0.8 on the Plus ROM with MacTCP 2.0.6 and the Dayna driver;
  Open Transport and PPC are not exercised.
- u26 itself: the container flags follow its conventions but this has only been
  run under Colima (arm64 VM). `/dev/net/tun` and `NET_ADMIN` must be allowed
  there for the tap.
- R4 asserts discovery and connection (logs, one TCP connection) and a human
  looks at the Mac lobby screenshot; it does not start a game.
