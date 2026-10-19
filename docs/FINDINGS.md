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
