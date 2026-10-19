# Building maxivmac

## Source Layout

```
src/core/        — Core emulation (machine config, main loop, extensions)
src/cpu/         — 68000/68020 CPU emulator and instruction decode tables
src/devices/     — Hardware device emulation (VIA, SCC, IWM, SCSI, ADB, etc.)
src/debugger/    — Interactive debugger, scripting, breakpoints
src/guest/       — Guest memory introspection (dialog detection, etc.)
src/storage/     — Host file system, AppleDouble, drive manager
src/lang/        — Type registry and global registry
src/platform/    — Platform backend (ImGui + SDL3 for windowing/audio)
  common/        — Shared platform code: event queue, keyboard map, disk I/O
src/config/      — Build configuration headers and Mac file format
src/util/        — Utilities (MacRoman encoding, etc.)
src/resources/   — Application icon resources
```

## Quick Start (macOS)

The convenience script builds everything in one step:

```bash
./build-macos.sh
```

This is equivalent to:

```bash
cmake --preset macos
cmake --build --preset macos
```

The binary is at `bld/macos/maxivmac`. To clean, remove the `bld/` directory.

## Running

The emulator requires a ROM file and at least one disk image:

```bash
# Mac II (default)
./bld/macos/maxivmac --rom=MacII.ROM disk.hfs

# Mac Plus
./bld/macos/maxivmac --model=MacPlus disk.img
```

ROM auto-detection searches `./`, `roms/`, and `--romdir` for `<MODEL>.ROM`.

### Data files

Definition files under `data/` (`data/debug/traps.def`, `types.def`,
`globals.def`, `errors.def`, `typemap.def`, `data/roms/`) are found
relative to the current directory first, then via `$MAXIVMAC_ROOT`, the
executable's location (`<exe>/../..`), and finally the source tree the
binary was built from.  Scripted runs can therefore start from any
directory.  If `traps.def` cannot be found the emulator prints a warning
(without it trap names and `wait text` do not work).

### Command-Line Options

| Flag | Description |
|------|-------------|
| `--model=MODEL` | Mac model: `MacPlus`, `MacSE`, `MacII`, `MacIIx`, `Classic`, `PB100`, `SEFDHD`, `Mac128K`, `Mac512Ke`, `MacPlusKanji`, `Twig43`, `Twiggy` (default: `MacII`) |
| `--rom=PATH` | Path to ROM file (auto-detected from model if omitted) |
| `--romdir=DIR` | Directory to search for ROM files |
| `--ram=SIZE` | RAM size: `1M`, `2M`, `4M`, `8M` (default: model-specific) |
| `--screen=WxHxD` | Screen geometry: `512x342x1`, `640x480x8`, etc. (D = log₂ bpp) |
| `--speed=N` | Emulation speed: `0`=1×, `1`=2×, `2`=4×, `3`=8×, `4`=16×, `5`=32× |
| `--scale=N` | Window scale factor (default: 2) |
| `--fullscreen` | Start in fullscreen mode |
| `--headless` | Run without GUI (for testing/automation) |
| `--silent` | Disable audio output |
| `--shared=PATH` | Mount host directory as shared drive (repeatable) |
| `--serial-a=MODE` | Modem port backend: `loopback`, `file:tx=PATH[,rx=PATH]`, `pty`, `slip` |
| `--serial-b=MODE` | Printer port backend (same modes as `--serial-a`) |
| `--net-redir=SPEC` | Port forward for slirp: `tcp:[hostip:]hostport:guestip:guestport`; listens on 127.0.0.1 unless `hostip` is given (`0.0.0.0` = every interface). `--slip-redir` is an alias that, as in upstream, listens on all interfaces |
| `--scsiN=TYPE` | SCSI target at ID N (0-6): `daynaport[,multiframe]`, `none` (not on the Mac II family) |
| `--eth=SPEC` | DaynaPORT Ethernet backend: `slirp[,hostfwd=tcp:[HOSTIP]:HOSTPORT-GUESTIP:GUESTPORT]`, `hub:HOST:PORT[,key=FILE]` (ethhub LAN; FILE = the hub's shared secret), `none`. Default `slirp`; `none` with `--headless`/`--dbg-script` (give `--eth=slirp` for a live link there). Forwards listen on 127.0.0.1 unless HOSTIP is given |
| `--eth-mac=MAC` | DaynaPORT MAC address (random on a hub unless given) |
| `--eth-latency=D` | Minimum delay of received frames on the emulated clock (default `10ms`, `0` = off) |
| `--debugger` | Start with debugger prompt (paused at first instruction) |
| `--dbg-script=FILE` | Execute `.dbg` script at debugger startup (repeatable) |
| `--debugserver[=PATH]` | Start debug server on Unix socket |
| `--trace-traps` | Enable A-line trap tracing to stderr |
| `--diag=LIST` | Enable diagnostic traces (comma-separated: `extfs`, `guest`, `sd`, …) |
| `--record=PATH` | Record golden file for non-regression testing |
| `--verify=PATH` | Verify against golden file (exit 0=pass, 1=fail) |
| `--trace=PATH` | Write CPU+IO text trace to file |
| `--trace-cpu=PATH` | Write CPU-only text trace to file |
| `--title=TEXT` | Window title |
| `-h`, `--help` | Show help |
| positional args | Disk image paths |

## Requirements

- **CMake** ≥ 3.20
- **Ninja** (recommended) or Make
- **SDL3** development libraries
- **OpenGL** (provided by the OS on macOS/Windows; Mesa on Linux)
- **macOS:** Xcode command-line tools (provides Clang)
- **Linux:** X11 development libraries (optional, for X11 support via SDL)
- **Windows:** MinGW or MSVC

Optional:
- **libslirp** — enables TCP/IP networking via `--net-redir` (auto-detected at configure time)

## Build Presets

| Preset | Platform | Type | Notes |
|--------|----------|------|-------|
| `macos` | macOS | Release | Default macOS build |
| `macos-coverage` | macOS | Debug | Clang source-based code coverage |
| `macos-asan` | macOS | Debug | AddressSanitizer + UndefinedBehaviorSanitizer |
| `linux` | Linux | Release | Default Linux build |
| `windows` | Windows | Release | MinGW/MSYS2 |

```bash
cmake --preset <preset>
cmake --build --preset <preset>
```

## Manual Configuration

```bash
cmake -B bld -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build bld
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MINIVMAC_ABNORMAL_REPORTS` | `0` | Enable abnormal-condition reports |
| `MINIVMAC_LOCALTALK` | `0` | Enable LocalTalk emulation |
| `MAXIVMAC_COVERAGE` | `OFF` | Clang source-based coverage instrumentation (use `macos-coverage` preset) |
| `MAXIVMAC_SANITIZE` | `OFF` | AddressSanitizer + UBSan (use `macos-asan` preset) |

## Static analysis

`tools/static-analysis.sh` (clang-tidy, cppcheck, shellcheck, ruff, lizard against a
ratcheted baseline); see [STATIC_ANALYSIS.md](STATIC_ANALYSIS.md).

## Building the INIT (macOS guest code)

`build-init.sh` compiles the classic Mac INIT that runs inside the emulated guest OS. It stamps the current git version into `macsrc/init/version.h`, then launches the emulator to drive THINK C 5.0 via a `.dbg` automation script.

**Prerequisites:**
- A working emulator build at `bld/macos/maxivmac`
- `macsrc/build.mac` — a `.mac` bundle containing the THINK C 5.0 environment and the INIT project

```bash
./build-init.sh
```
