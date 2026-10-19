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
  fails 23 test cases (fixtures are looked up relative to the source tree);
  `ctest` passes because it sets `WORKING_DIRECTORY` to the source dir.
- **Repro:** `cd <builddir> && ./tests`.
- **Status:** open (harmless under ctest). Not resolved by the data-path fix
  (S2), which covers `data/` definition files only; the 26 failing cases
  still look for test fixtures relative to the source tree.
