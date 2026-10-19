# Findings log

Bugs and drift noticed in existing code while working on something else.
Entries are grouped by the work that found them. Most were not fixed in
the branch that found them.

## Scripting (found while building the headless script tests)

### S1. Docs were ahead of the code: time suffixes and `set default-timeout`

- **Symptom:** `docs/features/SCRIPTING.md` documented time suffixes, but
  `20s` was tokenised as the number 20 followed by a word, so `wait for 20s`
  waited 20 units. Even once parsed, the conversion assumed unscaled 8 MHz
  cycles, roughly 500x too short. `set default-timeout` never matched
  because the tokenizer split the hyphenated name. The old default was 40M
  units, which is 80 ms of emulated time.
- **Status:** fixed in cfab582 (as 58b540b on this branch). The default is
  now 60 s of emulated time and the time unit is defined in the docs.

### S2. `wait text` was dead: data files loaded relative to the cwd

- **Symptom:** `data/debug/traps.def` and the other definition files were
  opened relative to the current directory, so the emulator loaded 0 traps
  when started anywhere else and `wait text` / `break text` never fired. The
  docs also claimed OCR, which is not what happens.
- **Status:** fixed (0e1c1b5 / 3f585ef; 0689553 / 9698405 on this branch).
  `ResolveDataPath()` finds the files, and the docs now say what text
  capture sees (QuickDraw string traps, plus bare Pascal strings).

### S3. Quick mouse clicks never open menus

- **Symptom:** the mouse device handles one event per 60 Hz tick and ignores
  input for 4 ticks after a button event, so a quick click is a blip and
  menus never open.
- **Status:** fixed by tick-spaced gestures (f2a286d; c38d8a1 on this
  branch).

### S4. Open gaps and not verified

- `commands ... end` blocks read the console, not the script (exit 2 in
  unattended mode).
- `wait text` only sees text drawn after it was armed; the menu bar, drawn
  once at boot, is missed if armed late.
- Fixed `wait for` delays are fragile; synchronise on drawn text instead.
- Key presses shorter than a tick can be missed by polling software.
- Not run: `launch`, `exittoshell` and `shutdown` (need the SharedDrive
  INIT); `wait screen` and `test/scripts/screen_match.dbg` (need
  `test/fixtures/desktop.png`, which is absent); models other than the Plus.
- The `data/` source-directory fallback is baked into the binary.

## 2026-10: found during feat/scsi-bus

### F1. Legacy SCSI stub: SEL check only matches ICR == 0x04

- **Symptom:** `SCSI_Check` tests `(ICR >> 2) == 1`, true only when the
  whole ICR value is exactly 0x04. SEL combined with any other ICR bit
  (BSY, ATN, data bus) is not seen.
- **Repro:** `ncr5380 legacy: SEL assertion quirk only triggers when ICR == SEL`
  in `test/test_ncr5380.cpp`.
- **Status:** open, intentionally preserved. The no-target path must stay
  bit-identical because the boot ROM probes it and the golden CRCs depend on
  it. The real bus (target attached) does not have this quirk.

### F2. Legacy SCSI stub: arbitration "doesn't really work"

- **Symptom:** the stub reports AIP unconditionally when ODR bit 7 and MR
  bit 0 are set, and never raises BSY, so every selection times out. The
  stub's own comment says so.
- **Status:** by design for the no-target case; superseded by the real bus.

### F3. Mac II SCSI address check flags A9

- **Symptom:** in `machine.cpp` the II-family `addr & 0x1F8F` check
  reports `kMACH_SCSI_nonstandard_address` for any access with A9 set.
- **Status:** open, not changed (reports only; no behavioural effect). The
  II family uses a different pseudo-DMA scheme that is not implemented.

### F4. test/README.md and verify.sh disagree

- **Symptom:** the README says ROMs are in `roms/` and that `record.sh`
  covers Plus and II; `verify.sh` reads `data/roms/` and there are six
  golden files.
- **Status:** open (docs drift).

### F5. Scripting docs ahead of the code

- **Symptom:** `docs/features/SCRIPTING.md` documented time suffixes such as
  `wait for 5s` and `wait text`; the implementation only accepted raw cycle
  counts.
- **Status:** fixed; see S1 and S2 in the Scripting section above.

### F6. The `tests` binary depends on the working directory

- **Symptom:** running the `tests` executable from the build directory
  fails 26 test cases (fixtures are looked up relative to the source tree);
  `ctest` passes because it sets `WORKING_DIRECTORY` to the source dir.
- **Repro:** `cd <builddir> && ./tests`.
- **Status:** open (harmless under ctest). Not resolved by the data-path fix
  (S2), which covers `data/` definition files only; those cases
  still look for test fixtures relative to the source tree.

## 2026-10: found during feat/daynaport

### F7. Real Dayna driver: no INQUIRY at boot, READ(1) is the probe

- **Symptom:** at boot the System 6.0.8 Dayna driver only selects the target
  and sends `READ` with allocation 1 (twice), expecting CHECK CONDITION. The
  INQUIRY, ENABLE, GET MAC, SET MULTICAST sequence happens only when the user
  picks EtherTalk in the Network control panel.
- **Repro:** `--scsi4=daynaport --diag=scsi`, boot the Dayna image, open
  Control Panel > Network and choose EtherTalk (trace in
  `docs/features/NETWORKING_DAYNAPORT.md`).
- **Status:** documented; the target handles it (donor behaviour kept).

### F8. Real Dayna driver frames WRITE with a header and a trailing zero record

- **Symptom:** `WRITE` CDB `0A 00 00 00 LL 80` carries LL = frame length, but
  the data phase is `[00 LL 00 00][frame][00 00 00 00]`, i.e. 4 + LL + 4
  bytes in two or three DATA OUT records. A target that stops after LL bytes
  would leave the driver waiting.
- **Repro:** the same trace; AARP probes are 50 bytes (`0x32`).
- **Status:** handled (the donor's record-framed write is correct).

### F9. Vendor opcode 0x80 and the group-4 CDB length

- **Symptom:** the NCR5380 model took CDB length from the opcode group only,
  so a 6-byte vendor command with opcode 0x80 (group 4, normally 16 bytes)
  hung in COMMAND phase.
- **Repro:** `ncr5380 bus: a target can override the CDB length of a vendor
  opcode`.
- **Status:** fixed in this branch (`ScsiTarget::cdbLength`).

### F10. Scripting: old `click x y N` three-argument form is gone

- **Symptom:** scripts written for the pre-fix scripting commands
  (`click 331 259 50000000`) fail with `expected
  'hold <duration>'`; menu selection needs the Apple menu offsets seen in
  screenshots (Chooser y=123, Control Panel y=128 with `drag`), not the item
  centres.
- **Status:** resolved by integration: the scripting changes are in this
  history, so the old form no longer exists. Scripts must use the new
  gesture syntax documented in `docs/features/SCRIPTING.md`.

### F11. The emulator writes to disk images passed on the command line

- **Symptom:** an end-to-end script that opens the Chooser and leaves it
  running changes the HFS image; the next run of the same image failed to
  boot (flashing "?" disk). Always run guest tests on a fresh copy.
- **Status:** open (by design; worth a read-only option).

### F12. libslirp timers are stored but never fired

- **Symptom:** `SlirpStack` (as `serial_slirp.cpp` before it) records timer
  expiries from `timer_mod` but nothing calls `slirp_handle_timer`; only the
  built-in fast/slow TCP timers inside `slirp_pollfds_poll` run. IPv6 router
  advertisement timers are the known user, which is disabled.
- **Status:** fixed: `SlirpTimers` keeps the table and `SlirpStack::poll()`
  fires the due timers (`timer_mod` is in milliseconds, `clock_get_ns` in
  nanoseconds).

## 2026-10: review pass on the DaynaPORT branch

### F13. A READ too small for the head frame wedged receive

- **Symptom:** the first queued frame that did not fit the READ's allocation
  length got CHECK CONDITION and stayed queued; the real driver never sends
  REQUEST SENSE and retries with the same length, so receive stalled for good.
  (The driver's own READ is 1524 bytes, enough for a maximum frame, so it was
  latent.)
- **Status:** fixed: the frame is dropped, counted in `rxOversize`, and the
  READ returns the empty six-byte header.

### F14. Frames dequeued for a READ are lost when the READ is cut short

- **Symptom:** a bus reset or ABORT in the middle of a READ's data phase loses
  the frames already taken from the queue.
- **Status:** counted (`rxLost`), not recoverable; documented.

### F15. ethhub and slirp forwards were reachable from the network

- **Symptom:** ethhub listened on 0.0.0.0 (any source sending a hello became a
  client, broadcasts flood to all clients: UDP reflection, MAC capture) and
  slirp host forwards bound INADDR_ANY.
- **Status:** fixed: both default to 127.0.0.1; wider exposure is explicit
  (`--listen ADDR:PORT` with a warning and `--allow CIDR`; `hostfwd=tcp:HOSTIP:...`).
  The ethhub container still listens on 0.0.0.0 inside (Docker publishing needs
  it) and is published on 127.0.0.1.
- **Deferred:** ethhub has no authentication and no per-client rate limit.

### F16. A live slirp link made scripted runs non-deterministic

- **Symptom:** with `--scsiN=daynaport` and no `--eth` the default was slirp,
  which runs on the host clock and real sockets.
- **Status:** fixed: `--headless` / `--dbg-script` without `--eth` get `none`
  (cable out) with a note; interactive runs keep slirp; an explicit
  `--eth=slirp` always works.

### F17. NCR5380: /DACK and ATN

- **Symptom:** pseudo-DMA was only recognised on register 0 (the chip ignores
  A0-A2 with /DACK), and ATN was only honoured during selection.
- **Status:** fixed: any access with /DACK is the data register; ATN during
  COMMAND/DATA/STATUS gives MESSAGE OUT at the next REQ, then the phase
  resumes (so ABORT after a timeout works). Unit tests only; the real driver
  never asserts ATN after selection in the traces seen so far.

### F18. Multi-frame READ and the "more" flag are unverified

- **Symptom:** packing several frames per READ and flag value 0x10 come from
  the Mini vMac fork; BlueSCSI returns one frame per READ, and PiSCSI's
  `e_more_data_available` may be 0x00000001.
- **Status:** packing is opt-in (`--scsiN=daynaport,multiframe`), default one
  frame per READ. **Open:** test with the real driver and two frames pending.

### F19. Mac II family SCSI targets are not supported

- **Symptom:** /DACK (A9) is decoded on the compact Macs only; the II family
  uses separate pseudo-DMA handshake regions, so a driver could not transfer
  data.
- **Status:** `--scsiN` is refused there with an error. **Deferred:** decode
  the II-family handshake regions.

### F20. Not done / not verified in this pass

- Two DaynaPORTs each get their own slirp (same guest address, same forwards);
  documented, not detected.
- The `--eth=hub` backend (join an ethhub from the emulator) is a separate task.
- The MacTCP ping end to end was run on the pre-review `integration` build by
  a different task; the defaults changed here (one frame per READ, `none` in
  headless runs) were checked with unit tests, golden files and the script
  tests only.
- The upstream issue or pull request for the donor fork, if any, is unknown.
- Colima's ssh port forwarder does not carry UDP from the macOS host to a
  container, so the host-to-container path of ethhub was not exercised (see
  `docs/features/ETHHUB.md`).

## 2026-10: MacTCP ping follow-up

### F21. Restart left the guest without a disk

- **Symptom:** after Special > Restart (or any reset) the Mac Plus showed the
  flashing "?": `DoMacReset()` ejects every image and nothing put them back.
  This blocks every flow that needs a restart to load a driver (MacTCP,
  EtherTalk).
- **Status:** fixed: the startup images (command line or `.mac`, drive order)
  are re-inserted after the reset (`Sony_RestoreStartupDisks`). Applies to the
  host Reboot action too. Not restored: images the user added later. Covered
  by `test/scripts/restart_keeps_disk.dbg` (fails without the fix: the boot
  never gets past the "?"). See `src/devices/SONY.md` 5.6.
