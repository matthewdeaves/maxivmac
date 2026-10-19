# Findings log

Bugs and drift noticed in existing code while working on something else.
Not fixed in the branch that found them.

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

- **Symptom:** `docs/features/SCRIPTING.md` documents time suffixes such as
  `wait for 5s` and `wait text`; the implementation only accepts raw cycle
  counts. A cold boot to the Finder is about `wait for 150000000000`.
- **Repro:** run a `--dbg-script` using `wait for 5s`.
- **Status:** open (found by an earlier spike, re-confirmed that raw
  cycle waits are what work).

### F6. The `tests` binary depends on the working directory

- **Symptom:** running the `tests` executable from the build directory
  fails 23 test cases (fixtures are looked up relative to the source tree);
  `ctest` passes because it sets `WORKING_DIRECTORY` to the source dir.
- **Repro:** `cd <builddir> && ./tests`.
- **Status:** open (harmless under ctest).

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

### F10. Scripting: old `click x y N` three-argument form is gone upstream

- **Symptom:** scripts written for the pre-fix scripting commands
  (`click 331 259 50000000`) fail on `fix/script-commands` with `expected
  'hold <duration>'`; menu selection needs the Apple menu offsets seen in
  screenshots (Chooser y=123, Control Panel y=128 with `drag`), not the item
  centres.
- **Status:** open (belongs to the scripting branch).

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
- **Status:** open, behaviour unchanged by the refactor.
