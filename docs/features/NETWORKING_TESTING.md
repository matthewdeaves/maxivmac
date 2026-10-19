# Testing the network end to end: MacTCP ping

`test/net/` runs a real MacTCP ping from System 6.0.8 through the emulated
DaynaPORT and slirp, headless, with no manual steps. It is the check that the
whole stack (SCSI bus model, DaynaPORT target, backend, receive latency, guest
reset keeping its disk) works with Apple's own software.

```sh
export MAXIVMAC_DAYNA_INSTALLER=~/Downloads/DaynaPORT_Installer.img_.sit
export MAXIVMAC_MACTCP=~/Downloads/mactcp206.sit_.bin
export MAXIVMAC_PING=~/Downloads/MacTCP_Ping_2.0.2.sea_.bin
test/net/run-ping.sh bld/macos/maxivmac          # or: ctest -R net_ping
```

About 10 s to prepare the disk (once, cached) and 50 s per run on an M-series
Mac at the emulator's maximum headless speed.

## Inputs (none of them is in the repository)

Apple, Dayna and third-party software is not redistributed here. Every input
is an environment variable; if one is missing the script (and the ctest
`net_ping`) **skips** with exit code 77 and says which one.

| Variable | What | Default |
|---|---|---|
| `MAXIVMAC` | emulator binary | `bld/macos/maxivmac` |
| `MAXIVMAC_ROM` | Mac Plus ROM | `data/roms/MacPlus.ROM` |
| `MAXIVMAC_BASE_DISK` | bootable System 6.0.8 HFS image (the one the golden tests use) | `extras/disks/608.hfs` |
| `MAXIVMAC_DAYNA_INSTALLER` | the Dayna "DaynaPORT Installer": the `.sit`, the NDIF `.img` inside it, or the decoded HFS floppy | none |
| `MAXIVMAC_MACTCP` | MacTCP 2.0.x: the file `MacTCP` or the archive holding it | none |
| `MAXIVMAC_PING` | MacTCP Ping 2.0.x: the file `MacTCP Ping` or its archive | none |
| `MAXIVMAC_NET_CACHE` | cache of the prepared disk | `$TMPDIR/maxivmac-net` |
| `MAXIVMAC_NET_DISK` | the prepared disk (reused when it exists; delete it to rebuild) | `$MAXIVMAC_NET_CACHE/net-disk.hfs` |
| `MAXIVMAC_OUT_DIR` | screenshots, the assembled script, the SCSI trace | `$MAXIVMAC_NET_CACHE/ping-out` |
| `MAXIVMAC_ARGS` | extra emulator arguments (e.g. `--eth-latency=0`, the control run) | none |
| `MAXIVMAC_HFSUTILS` | directory with `hmount`, `hcopy`, ... if not on `PATH` | none |

Host tools: **hfsutils** (`hmount`, `hcopy`, `hls`, `humount`), **unar/lsar**,
python3 and **macOS** (`xattr`, `..namedfork/rsrc`: `unar` keeps a Mac file's
resource fork and Finder info in extended attributes, and
`tools/netdisk/macbinary.py` reads them back to feed `hcopy -m`).

## Disk preparation (`test/net/prep-disk.sh`)

1. **Decode the installer.** The Dayna download is a StuffIt archive holding an
   NDIF disk image (`DaynaPORT Installer.img`, block map in its resource fork,
   ADC-compressed chunks). `tools/netdisk/ndif.py` turns it into a 1.4 MB HFS
   floppy; `unar` unpacks the `.sit` first.
2. **Run the Dayna installer in the emulator**
   (`scripts/dayna-install.dbg`): the base disk is the boot disk, the floppy
   the second disk. The script opens the floppy, starts *Installer*, chooses
   *Custom Install*, ticks "AppleTalk v58.0.0 for System 6" and "DaynaPORT
   SCSI/Link", installs and restarts (which needs the reset to keep the disks,
   see below). This puts `Network`, `AppleTalk`, `EtherTalk Phase 2` and
   `Responder` into the System Folder and grows the System file.
3. **Host side:** MacTCP goes to the **root** of the volume, MacTCP Ping into
   `Net Tools` (hfsutils + MacBinary).

### Order matters: EtherTalk first, MacTCP after

With MacTCP in the System Folder **at boot**, switching to EtherTalk in the
Network control panel fails with "Could not switch to EtherTalk due to an
error". The trace shows `Open ".ATP"` returning -108 (`memFullErr`): the System 6
system heap has 27.9 KB free with MacTCP installed (33 KB without it) and a
complete EtherTalk bring-up needs about 31.4 KB. It is not a RAM-size problem
(1, 2 and 4 MB give the same heap) and not an emulator regression. Padding the
system heap, a `sysz` resource, and deleting other INITs did not help.

So the disk is prepared with MacTCP **outside** the System Folder and the ping
run switches to EtherTalk first, and only then drags MacTCP into the System
Folder and restarts. (Installing MacTCP before switching, as one would
naturally do, does not work.) InterSLIP is left out for the same reason (it
takes another KB of heap and is not needed).

## The ping run (`test/net/run-ping.sh`)

One emulator session on a fresh copy of the prepared disk, assembled from the
blocks in `scripts/ping/` (`--scsi4=daynaport --eth=slirp --diag=scsi`, the
default `--eth-latency`, no environment tricks):

1. boot to the Finder (`wait text "Special"`);
2. Chooser: AppleTalk **Active**;
3. Control Panel > Network: **EtherTalk** (waits for "Switching your
   AppleTalk connection" and the panel redraw);
4. drag MacTCP from the volume root into the System Folder;
5. Special > **Restart** (the startup disk is re-inserted; `wait text
   "Welcome to Macintosh"` then the Finder menu bar);
6. Control Panel > MacTCP: **Ethernet Built-In**, More...: manual, 10.0.2.15,
   255.255.255.0 (slider), gateway 10.0.2.2, DNS 10.0.2.3 (domain
   `example.net`: MacTCP refuses a DNS entry with a blank domain); close
   (writes `MacTCP Prep`);
7. Special > **Restart** again, so MacTCP loads;
8. open `Net Tools`, start MacTCP Ping, ping **10.0.2.2** (the slirp gateway),
   5 packets, and `wait text "5/5/0/0"` (its tally, "out/in/bad/%loss").

It passes when the script ran to the end (every `wait` met), the SCSI trace
holds 5 echo requests (98-byte DATA-OUT records) and 5 echo replies (108-byte
READ records), and the tally was drawn. The EtherTalk choice lives in PRAM and
PRAM is not kept between runs, so steps 2-8 are always one session; a prepared
disk cannot carry it.

Waiting on text rather
than fixed delays means a slow host does not break the run; the remaining fixed
waits are short settling times after a click (mouse gestures need whole ticks,
and the cursor lags).

### Control runs

`MAXIVMAC_ARGS=--eth-latency=0 test/net/run-ping.sh` fails: Ping shows
`success` for echo 1 and `timeout` for the rest, and the final `wait text` times
out (exit 1). The 2-4 ms the guest takes to come back for a reply is too short;
see "Receive latency" in `NETWORKING_DAYNAPORT.md` for the measured table
(fails below 6 ms, default 10 ms).

## Evidence kept with this change

Screenshots and traces of the run on the author's machine (not in the
repository): `scratchpad/ping2/` of the session that produced it: `run4/`
(first full run from a freshly prepared disk), `run5/` (second run, identical
final screenshot), `control-nolatency/`, `lat<N>ms/` (latency sweep).

## Not covered

- **TCP**: there is no TCP client on the disk (Net Tools only has MacTCP Ping),
  so TCP through slirp and host forwards are not exercised by this test.
- DNS name resolution (the DNS server is configured and reachable, a name was
  never resolved), ICMP to hosts beyond slirp's gateway/DNS addresses.
- Real-time (`--speed=0`) behaviour; the run uses the maximum headless speed,
  where guest-side timeouts (Ping's 60-tick interval) take about 25 s of
  emulated time each.
- Other MacTCP clients (does 10 ms of receive latency suit them all?).

## Scripting notes

Found while writing the scripts (also in `docs/FINDINGS.md`): `source` inside a
script loses the lines after the first `wait` in the scratch experiment, so the
blocks are concatenated by `run-ping.sh` (not re-checked on this tree);
`drag` to a menu item lands one row off because the cursor lags, so menus are
driven with `mousedown`, `move`, `mouseup` and waits; a Finder double-click
needs the legacy `click x y`, `wait for 1s`, `click x y` form (`hold` makes
it too slow); a pop-up menu is chosen with a drag followed by a click; the
installer window repaints several seconds after its text is drawn, so a
screenshot straight after `wait text` can show the previous window.
