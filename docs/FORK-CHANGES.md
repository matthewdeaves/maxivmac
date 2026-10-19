# What this fork changes (for upstream users)

Everything below is additive or a deliberate, listed behaviour change. Existing
command lines, `.mac` files and golden files keep working (`--slip-redir`
still listens on every interface, `--speed=255`/`max` still works).

## New (one command each)

| Feature | Try it |
|---|---|
| DaynaPORT SCSI Ethernet on any SCSI ID | `maxivmac --model=MacPlus --scsi4=daynaport --eth=slirp disk.hfs` |
| Shared virtual LAN (UDP hub) | `tools/ethhub/ethhub &` then `maxivmac --scsi4=daynaport --eth=hub:127.0.0.1:5555 ...` |
| Port forward with optional host IP | `--net-redir=tcp:[0.0.0.0:]8080:10.0.2.15:80` |
| Eth latency model | `--eth-latency=2ms` |
| Debugger scripts (headless) | `maxivmac --headless --model=MacPlus --dbg-script=test/scripts/boot_wait.dbg disk.hfs` |
| Debug socket | `maxivmac --debugserver=/tmp/mv.sock ...` |
| Startup disks survive a guest Restart | `maxivmac disk.img` then Special > Restart (SE and later) |

## Behaviour changes

| Area | Now | Was |
|---|---|---|
| `--speed` / `--scale` | whole number in range (speed 0..5, 255 or `max`; scale 1..16), else warning and ignored | `atoi`, anything went through |
| `--headless` | needs `--model=` or a `.mac` file (exit 2 otherwise) | -- |
| script `key` | down for one tick; `key ... hold <duration>` for longer | -- |
| script errors | a bad or unknown command exits 2 (including `build-init.sh` scripts in the GUI build) | -- |
| `wait` default timeout | 60 s emulated (`set default-timeout`) | -- |
| Restart | re-inserts the startup disks | disks were gone |
| debug socket | mode 0700 (umask 0077 around bind); SIGPIPE ignored | default umask |
| SLIP decoder | an unterminated frame is capped at 64 KiB, then resynchronises | unbounded |
| extfs | one guest read/write capped at 64 MiB | unbounded |
| `--net-redir` / `--eth=slirp` forwards | loopback unless a host IP is given | -- |
| `--slip-redir` | all interfaces, as upstream (a warning is printed) | same |

## New optional build dependencies

- libslirp >= 4.9: networking only; without it networking is compiled out.
- python3: the ethhub tests (skipped without it).
- a sanitizer runtime (ASan/UBSan): only the `ethhub_asan_build` ctest step;
  the default build no longer needs it.

`ctest -LE net` skips the slow end-to-end ping tests (label `net`).

## Experimental and known limits

- `daynaport,multiframe` is unverified against real software.
- `--eth-latency` is tuned on MacTCP Ping only.
- ethhub has no authentication and no rate limit: keep it on loopback or a trusted LAN.
- The Mac SE needs `--speed=0` for AppleTalk start-up (FINDINGS F41).
- A slirp guest can reach the host's loopback (FINDINGS F38).
- Windows builds are not verified by the author.

## How it was verified

`ctest` (unit tests, ethhub tests against an ASan build), `test/verify.sh`
(the 6 golden files, unchanged), `test/run_script_tests.sh`,
and end-to-end MacTCP ping on a Plus and an SE (`docs/features/NETWORKING_TESTING.md`).
