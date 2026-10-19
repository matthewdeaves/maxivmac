# Findings log

Bugs and drift noticed in existing code while working on something else.

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
