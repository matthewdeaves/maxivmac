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
  unattended mode). Deliberately not changed: `on text` and `if text`
  (S8) cover the script use.
- A plain `wait text` still only sees text drawn after it was armed (by
  design, see S6); `wait text ... recent` reaches back.
- Fixed `wait for` delays are fragile; synchronise on drawn text instead.
- `type` still uses very short key presses (80,000 units); text entry via
  key events works, but a polling reader would miss them (S9).
- Not run: `launch`, `exittoshell` and `shutdown` (need the SharedDrive
  INIT); `wait screen` and `test/scripts/screen_match.dbg` (need
  `test/fixtures/desktop.png`, which is absent); models other than the Plus.
- The `data/` source-directory fallback is baked into the binary.

### S5. `source` in a script, and several `--dbg-script` files, lost lines

- **Symptom:** a `wait` inside a file run by `source` (or by the first of two
  `--dbg-script` files) saved only that file's remaining lines; the caller's
  loop then overwrote them with its own, so everything after the first
  `wait` in the sourced file vanished, and the script reported success. The
  second `--dbg-script` file also started immediately, before the first
  one's waits had finished, and the first file reaching its end exited 0
  before the second ran. `test/net/run-ping.sh` worked around it by
  concatenating blocks.
- **Cause:** `executeCommands` was re-entered by `source` and each call
  owned the single pending-script slot.
- **Status:** fixed. `source` inside a running script splices the file's
  lines into the running script, and all `--dbg-script` files are read and
  run as one script (`SourceFiles`). Tests: "source keeps the lines after a
  wait" and "several --dbg-script files run in order" in
  `test/run_script_tests.sh`. Unquoted absolute paths (`source /a/b.dbg`)
  still fail: the tokenizer reads `/` as an operator, so quote the path.

### S6. `wait text` was blind to text drawn before it was armed

- **Symptom:** text drawn between two script lines (while the script was
  not waiting, e.g. an alert that appears during a `wait for`) could never
  be matched; scripts used fixed delays or pulled the menu down again to
  force a redraw.
- **Status:** fixed without changing the default. Every captured string is
  kept in a 256-entry ring with its emulated time; `wait text "x" recent
  [window]` consults it (default look-back 5 s), `clear text` empties it,
  `info text` shows it. A plain `wait text` keeps the old meaning, because
  letting it see old text would let a stale string satisfy a later step.
  Tests: `test/test_script_text.cpp` (ring), `text_recent*.dbg`,
  `text_not_recent.dbg`, `text_cleared.dbg`, `text_too_old.dbg` in
  `test/run_script_tests.sh`.

### S7. Poor failure messages

- **Symptom:** a timeout printed `TIMEOUT: wait (bp 3) expired ...` and
  nothing else: not which script line, not what the guest was doing. A
  `fail` printed its message only. With a 25-step script that is a screenshot
  and a guess.
- **Status:** fixed. Script lines now carry `file:line` (S5). Every wait
  timeout, `expect` failure and `fail` prints `at file:line: command` and the
  last ten drawn strings with their emulated times. New `expect text|screen`
  (a `wait` that reports as an assertion) and `log "message"` (timestamped,
  emulated time). The old `log` command (guest log) already existed, so
  `log` with a *quoted* first argument is the message form; that needed a
  `quoted` flag on tokenizer tokens. Tests: `expect_ok.dbg`,
  `expect_fail.dbg`, `fail_context.dbg`, `must_timeout.dbg` in
  `test/run_script_tests.sh`; `test/test_script_text.cpp` for the formatter.

### S8. No way to branch, or to handle an optional dialog

- **Symptom:** a script could not react: an optional alert ("Cannot open
  MacTCP driver") either had to be waited for (and failed the run when it
  did not appear) or was ignored (and wedged the next step). The documented
  `break text ... commands ... end` reactive form does not work in script
  files (S4) and, even from the prompt, stops the CPU at every match.
- **Status:** fixed with two small commands instead of parsing `commands`
  blocks out of script files: `if text "x" [recent [window]] then <cmd>
  [else <cmd>]` tests the recent-text buffer once, and `on text "x" [once]
  do <cmd>` registers a handler that runs on every matching draw without
  stopping the guest. Handlers may only run commands that do not resume the
  CPU (an allow list, checked when the handler is set up). Limits: `if`
  has one level (no nested `else`), the condition is text only (no screen or
  memory conditions), and `on` handlers are not removed by `delete`; use
  `on clear`. Not tried against a real guest alert; the guest tests use the
  Finder's menu text. Tests: `if_text.dbg`, `on_handler.dbg`, `on_fail.dbg`,
  `on_bad_wait.dbg`; token helpers in `test/test_script_text.cpp`.

### S9. Key presses were too short for software that polls the keyboard

- **Symptom:** `key` held a key for 80,000 units (0.16 ms emulated); the
  Mac's KeyMap (the polled keyboard state) is only looked at once per tick
  (16.6 ms), so a program using `GetKeys`/KeyMap never saw the press. Event
  based input (the Finder's rename field) was fine, which is why the tests
  passed. There was also no way to hold a key (a modifier at boot, a held
  arrow) or to press a modifier alone.
- **Status:** fixed. `key` now holds one tick by default, `key <spec> hold
  <duration>` holds as long as asked, and `shift`, `cmd`, `opt`, `ctrl` are
  accepted as keys by themselves. Measured on the Finder: with `key a` the
  KeyMap bit is set 12 ms after queueing; with `key a hold 1s` it is set
  after 0.5 s and clear after the release (`key_hold.dbg`,
  `key_plain_poll.dbg`). `type` is unchanged (typing text works through key
  events) and keeps its short presses. The timing plan is unit-tested
  (`test/test_script_keys.cpp`). The longer default press delays each `key`
  by about one tick (16.6 ms); the other scripts and the golden runs are
  unaffected (the goldens do not script keys).

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
- **Status:** fixed: `test/test_main.cpp` changes to the source directory
  (compile-time `MAXIVMAC_TEST_SOURCE_DIR`) before running, so `tests` gives
  the same result from any directory (579 of 579 from the build dir and /tmp).

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
- **Status:** default unchanged (by design); added `--disk-snapshot`: each image
  is copied once into an anonymous temporary file and the guest works on the
  copy (kept across eject/Restart within the run, gone at exit; the host file is
  never written, and "eject and delete" does not delete it). Covered by
  `test/test_disk_snapshot.cpp` and a script test.

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
  documented. A clashing host forward was already refused by the bind, but the
  message did not say why: it now names the likely cause, and a unit test
  checks that the second stack is refused. The two stacks still do not see
  each other.
- The `--eth=hub` backend (join an ethhub from the emulator) was a separate task: now F23.
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

### F22. MacTCP Ping loses replies that arrive before the send call returns

- **Symptom:** over `--eth=slirp` the first echo got a reply, echo #2 and later
  "timed out" although each reply was delivered: slirp answers synchronously,
  so the READ right after the WRITE already returns the reply (zero empty
  polls in the trace). The cause inside MacTCP / Ping was not found; a hold-off
  proved the race.
- **Status:** fixed in the emulator: received frames are held for a minimum
  time on the emulated clock (`--eth-latency`, default 10 ms; `EthLatency`).
  The first guess of 2 ms was not enough: the guest's next READ comes 2-4 ms
  after the WRITE, and the full MacTCP flow only passed from 6 ms up (table in
  `docs/features/NETWORKING_DAYNAPORT.md`). Whether 10 ms is enough for other
  MacTCP clients is not known; `--eth-latency=` raises it.

### F23. `--eth=hub:HOST:PORT` (EthHub backend)

- **Status:** added (`src/devices/eth_hub.*`). Link state is "socket open",
  not "hub alive" (UDP). Several emulators on a hub get distinct random MACs
  unless `--eth-mac` is given, because the default MAC is a constant and the
  hub would otherwise confuse the emulators silently. Verified against the
  real `ethhub` with three backends (unicast, broadcast, flooding); **not**
  verified: a guest actually talking across a hub (two emulators, or an
  emulator and the Docker peer), IPv6 hosts, a hub behind NAT.

### F24. MacTCP in the System Folder blocks the switch to EtherTalk

- **Symptom:** with MacTCP installed at boot the Network cdev says "Could not
  switch to EtherTalk"; `Open ".ATP"` returns -108 (memFullErr): 27.9 KB of
  system heap free against about 31.4 KB needed (33 KB without MacTCP).
- **Status:** worked around in the test disk prep: switch to EtherTalk first,
  then move MacTCP into the System Folder and restart (`NETWORKING_TESTING.md`).
  Not an emulator problem; a bigger system heap (not found) would be the cure.

### F25. The MacTCP ping runs end to end

- **Status:** `test/net/run-ping.sh` (ctest `net_ping`, skipped without the
  Apple/Dayna files) pings 10.0.2.2 from System 6.0.8 over DaynaPORT + slirp:
  5 out / 5 in, from a freshly prepared disk, including a guest Restart, no
  environment hacks. Needs F21 and F22 (without `--eth-latency` it is
  1 reply of 5). Not covered: TCP, DNS name resolution.

## 2026-10: fix/tests-cwd pass

### F26. `--headless` without `--model` crashed (null machine)

- **Symptom:** `maxivmac --headless --rom=... [--dbg-script=...]` segfaulted in
  `EmulatorShell::initMachine`. Without `--model` machine creation is left to
  the model selector (`SetLaunchConfig`), which a headless run never reaches,
  although `--help` says the default model is MacII.
- **Status:** fixed: exits 2 with "--headless needs --model=MODEL ... or a .mac
  file". Covered by `test/run_script_tests.sh`. Honouring the documented MacII
  default instead would be a behaviour decision; not taken here.
