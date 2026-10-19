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
  Control characters in listed text are escaped (`\r`, `\n`, `\t`) so each
  string is one line (the MacTCP Ping window draws rows ending in a CR).

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

### S10. `exit`, `screenshot` and several scripts: already there, now pinned down

- **Status:** `exit <code>` and multiple `--dbg-script` files turned out to
  exist (the latter broken, see S5); `exit` was undocumented apart from the
  status table. Added: `screenshot` prints the absolute path it wrote
  (`Screenshot saved: /abs/label.png`, relative names used to print the
  relative form), and tests for `exit 3`, `exit 0` and the printed path
  (`exit_code.dbg`, `exit_zero.dbg`, `shot_path.dbg`;
  `AbsoluteOutputPath` in `test/test_script_paths.cpp`). Documented `exit`.

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
- **Deferred:** ethhub has no authentication and no per-client rate limit
  (done: F43 to F46).

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

## 2026-10: fix/static-findings pass (clang-tidy, cppcheck, ASan/UBSan, fuzzing)

Found by a static-analysis and sanitizer sweep; the tooling is in
`tools/static-analysis.sh` (see `docs/STATIC_ANALYSIS.md`). Each fix is its own
commit on `fix/static-findings`.

### F27. `MAXIVMAC_SANITIZE` only instrumented the emulator

- **Symptom:** under the `macos-asan` preset `ctest` ran the unit tests, the
  icon/ad2bin/imgutil tools and the hub integration test unsanitized, so a
  sanitized `ctest` proved little.
- **Status:** fixed: `add_compile_options` / `add_link_options` for every
  target defined after the option.

### F28. `Debugger::trapHook` read `dregs[-1]` for stack-selector dispatch traps

- **Symptom:** `ParamLoc::Stack` is 0, so `idx = loc - 1` was -1 for
  SCSIDispatch, TEDispatch, Shutdown, ScriptUtil and the other dispatch traps
  whose selector is on the stack. Every headless script test aborted under
  ASan (6 of 16 failed).
- **Status:** fixed: register selectors only. Consequence (unchanged
  behaviour, now explicit): a subtrap breakpoint on a *stack*-selector
  dispatch trap never fires; reading the selector from the guest stack would
  need the caller's frame layout and was not done. 16 of 16 script tests pass
  sanitized. Not unit-testable (`debugger.cpp` is not linked into `tests`);
  the sanitized script tests are the test.

### F29. AppleDouble sidecar and resource-fork bounds checks wrapped in `uint32_t`

- **Symptom:** `entry.offset + entry.length` (and `mapOff + 28`, `dataOff +
  dataOff3`) wrapped, so a crafted `._file` passed the check and was then
  indexed far outside the buffer (host read of attacker-chosen offsets). The
  host-volume code parses sidecars of shared folders.
- **Status:** fixed in `src/storage/appledouble.cpp` and
  `tools/icon_extract/resource_fork.cpp` (64-bit arithmetic). Doctests with
  crafted inputs; the sidecar one aborts under ASan without the fix.

### F30. m68k: lazy X flag for ASR/ASL read the wrong operands; ROL.L signed shift

- **Symptom:** `NeedDefaultLazyXFlag{Asr,AslB,AslW,AslL}` read `LazyFlagArg*`.
  The X flag is evaluated lazily and has its own operand copy
  (`LazyXFlagArg*`, set next to `LazyXFlagKind`); a later instruction that sets
  the other flags but leaves X alone overwrites `LazyFlagArg*` first, so a
  later `ROXL`/`ROXR`/`ADDX`/`MOVE from CCR` could see a wrong X. In
  `ROL.L` with count 1, `1 << 31` shifted into an `int` sign bit (UB; same bits
  in practice).
- **Status:** fixed after reading the shift code (both sets are written with
  the same values, so the change only matters in the overwritten case, which
  is the bug). All six goldens verify unchanged, which says the golden runs do
  not hit the case; there is **no test that exercises it directly** (the CPU
  core is not linked into `tests`). Worth a CPU-level test if one is ever
  built.

### F31. Debugger number/duration parsers wrapped or kept garbage

- **Symptom:** a failed `ParseNumber` (`$`, `0x`, a literal over 64 bits) left
  `Token::numValue` uninitialised and the token was still a Number; hex and
  decimal literals silently wrapped; `ParseDuration` used `strtoull` without
  checking saturation and multiplied unchecked (`99999999999s` wrapped to a
  short or zero timeout); `FormatDuration` wrapped `cycles * 1000`;
  `ParseFmtSpec` overflowed `int` on a long count.
- **Status:** fixed: such tokens become Words (the command reports an error),
  durations that do not fit return false (`cannot parse timeout`), counts
  saturate at 2^20. Doctests, plus `test/scripts/huge_timeout.dbg`.

### F32. Debug server socket was world-connectable; client hang-up killed the emulator

- **Symptom:** `/tmp/maxivmac-dbg-<pid>.sock` was created with the process
  umask in a world-writable directory; the debugger can write guest memory
  and mount host paths. A client closing mid-reply raised SIGPIPE.
- **Status:** fixed: `umask(0077)` around `bind`, `SIGPIPE` ignored. Test checks
  no group/other bits and that the umask is restored. The server stays
  unauthenticated for the owning user.

### F33. SLIP decoder buffered without limit

- **Status:** fixed: 64 KiB cap; an oversized frame is dropped and the decoder
  resynchronises on the next END. Doctest.

### F34. extfs read/write allocated `ioReqCount` bytes

- **Symptom:** the guest's 32-bit request count sized a `std::vector` (up to
  4 GiB) on the host.
- **Status:** fixed: one call transfers at most 64 MiB; the short count is
  returned in the reply like any short read/write. Not covered by a test (the
  extfs glue needs a running guest).

### F35. `extn_clip` printf-style formatting took guest-sized widths

- **Status:** fixed: a run of more than 2 digits in a flag/width/precision is
  dropped (`%.2000000000x` took seconds). Consequence: widths above 99 are not
  honoured. No test (the glue needs a running guest).

### F36. `CpuSnapshot` reserved field was uninitialised

- **Status:** fixed (`CpuSnapshot snap{}`). The verifier never compares the
  field, and all six goldens verify. The shipped `.golden` files were **not**
  touched (and `record.sh` was not run): their reserved words are stack
  leftovers, so a fresh recording would differ from them in those 4 bytes per
  snapshot anyway.

### F37. Small hardening

- `--speed=` / `--scale=` (and the two-argument forms) used `atoi`: `abc` and
  out-of-range numbers were silently taken. Now whole-string, range-checked
  (speed 0..5, scale 1..16), with a warning and the default on error.
- Missing `<cerrno>` / `<array>` includes added (they compiled by accident).
- `test/debugger_smoke.sh` removed `/tmp/maxivmac-dbg-*.sock`, i.e. the sockets
  of every other running emulator; it now removes only its own.

### F38. slirp: the guest can reach the host's loopback (not changed)

- **Observation:** `slirp_stack.cpp` zeroes `SlirpConfig`, so
  `disable_host_loopback` is false and `restricted` is false: the guest reaches
  host services on 127.0.0.1 through the gateway address (10.0.2.2), and may
  talk to arbitrary hosts. That is how port forwarding and the MacTCP ping
  (F25) are set up and tested, and the emulator is a desktop tool, so the
  behaviour is left alone. Setting `disable_host_loopback = true` is the knob
  if the guest should not see host-local services; re-run `test/net/run-ping.sh`
  after changing it.

### F39. ethhub known limits (documented, no code change)

- Found by abuse scripts against `tools/ethhub/ethhub` (see
  `docs/features/ETHHUB.md`, "Known limits"): the 64-entry client table can be
  filled by 64 datagrams from other ports so a legitimate client is refused;
  one 1514-byte broadcast becomes 63 copies (63x amplification, reflectable
  with spoofed UDP source addresses); a client can claim another client's MAC
  by sending a frame with it as source, after which unicast to that MAC goes to
  the claimant. A fuzz run of random datagrams (seeds 11-13, ASan) found no
  crash. All of this assumes an attacker who can reach the UDP port; the hub
  defaults to loopback and has `--allow`.

### F40. Static analysis tooling and what is left

- **Status:** `tools/static-analysis.sh` runs clang-tidy, cppcheck, shellcheck,
  ruff and lizard against a ratcheted baseline; configs are the root, `src/`,
  `src/cpu/` and `test/` `.clang-tidy` files and `tools/cppcheck.suppressions`.
  See `docs/STATIC_ANALYSIS.md`.
- **Left open** (counts are in the baseline): the clang-tidy and cppcheck
  findings not listed above, shellcheck style findings, three ruff findings,
  and complexity (see the lizard numbers); `cert-err33-c` hits on unchecked
  `fwrite`/`fclose`/`sendto`/`recv`; the CPU core is intentionally exempt from
  most style checks.


## 2026-10: Mac SE DaynaPORT / MacTCP pass

The Mac SE is the primary real-hardware target, so the DaynaPORT + MacTCP ping
flow was run on `--model=MacSE` (`NETWORKING_TESTING.md`, `REAL_HARDWARE_SE.md`).
The SCSI side needed nothing: the NCR5380 decode, the Dayna probe and the
command mix are the same as on the Plus.

### F41. AppleTalk start-up on the SE bombs ("divide by zero") at any speed above 1x

- **Symptom:** on `--model=MacSE` with a DaynaPORT attached, Chooser > AppleTalk
  Active (and every later AppleTalk start) shows "Sorry, a system error
  occurred. divide by zero" with `--speed=1` or more (the headless default is
  16x). `--speed=0` (1x) works. Without a DaynaPORT it does not happen. Not seen
  on the Plus, where the routine below was never reached in the same flow.
- **Cause (partly verified):** the faulting `DIVU D5,D1` is in AppleTalk's
  loop-speed calibration: it primes a Time Manager task (`PrimeTime`, 60 s in
  microseconds), busy-loops with `DBF` for the ROM's `TimeDBRA` count (0x273 on
  the SE), calls `RmvTime` and takes the difference of the remaining counts as
  the elapsed time; the minimum of ten rounds is the divisor. With `--speed>0`
  most CPU time runs as "extra time" in which the VIA timers are frozen
  (`DoEmulateExtraTime`, `VIABase::extraTimeBegin`), so the Time Manager sees 0
  elapsed time and the divisor is 0. That the freeze is the reason follows from
  the speed dependence and the zero remaining-count difference; the guest code
  was read from a disassembly, not from source.
- **Status:** not fixed in the emulator (the freeze is how every speed above 1x
  works, and changing it would change the goldens). Worked around in
  `test/net/run-ping.sh`: `MODEL=MacSE` adds `--speed=0` unless
  `MAXIVMAC_ARGS` sets its own. It also means the fixed waits of the Plus
  script are 16 times longer in guest ticks on the SE (a double-click gap of 1 s
  is not a double-click there; the SE blocks use 150 ms). Real hardware is not
  affected. Anything that loops against the VIA timers at a speed-up may show
  the same kind of problem.

### F42. A Restart on the SE (and Classic, II) left the guest without a disk

- **Symptom:** after Special > Restart on `--model=MacSE` the ROM showed the
  flashing "?" for ever, so MacTCP/EtherTalk (both need a restart) could not be
  tested. The F21 fix did not apply: `DoMacReset()` (eject, zap, re-insert) only
  runs for models up to the Plus (`isCompactMac()`); on the others the guest's
  `RESET` only resets the devices and the ROM runs again, after the guest's
  shutdown had ejected the images.
- **Status:** fixed. `CustomReset()` calls `Sony_RestoreStartupDisks()` on those
  models (an image that is still inserted is skipped), and `SonyDevice::reset()`
  forgets the mount state so a still-inserted image is offered to the reopened
  driver as a pending insert again. The second part is by reasoning, not by a
  test of its own: in the observed case the Finder had ejected the images.
  Covered by `test/run_script_tests.sh` ("Mac SE: Restart boots from the same
  disk again", skipped without `MAXIVMAC_TEST_ROM_SE` / `data/roms/MacSE.ROM`);
  the unfixed build times out there.


## 2026-10: ethhub hardening

Follow-up to F15 and the static-analysis review of `tools/ethhub/ethhub.c`.
Each item has tests in `tools/ethhub/test/test_ethhub.py` (written first; they
fail against the old hub) and `test/test_eth_hub.cpp`.

### F43. ethhub tables: linear scans, and a hello flood could lock the client table

- **Symptom:** the MAC table (cap 4096) and the client table were linear scans,
  twice per frame; 64 hellos from 64 source ports filled the default client
  table and refused real clients until `--client-timeout`; one source IP could
  take every slot.
- **Status:** fixed. Both tables are chained hash tables (per-process hash
  seed) with an LRU list: O(1) lookup, learn, touch and eviction; ageing is
  O(expired). A full client table evicts the least recently seen client if it
  has been idle for `--live-window` (15 s), else refuses; `--max-per-ip`
  (default 16) caps one source IP. Memory is bounded by `--max-clients`,
  `--max-macs`. No behaviour change for the previous tests except that MAC
  eviction is now exact LRU (it was least `last_seen` in whole seconds, ties by
  index).
- **Not covered:** a hello flood from many (spoofed) source IPs still churns the
  idle entries of a table full of idle clients; clients with a keepalive are
  safe from it.

### F44. ethhub MAC hijack (any client could take any MAC)

- **Symptom:** the hub learned the source MAC of every frame, so a client could
  take over another client's MAC and receive its unicast traffic.
- **Status:** fixed with `--mac-move=deny` (default). A MAC owned by a live
  client (any datagram, keepalive hellos included, within `--live-window`) is
  not moved; the frame is dropped and counted (`dropped_macmove`). For TAP and
  uplink the entry's own age is used. `--mac-move=allow` is the old behaviour.
- **Decision:** default deny, because the only legitimate move is a restart on
  a new port, and that waits at most the live window after the old process
  stopped; a timer on the entry alone would let an attacker take a quiet
  guest's MAC and, with the spoofed frames refreshing it, lock the real owner
  out. Dropping (not forwarding without learning) keeps a spoofed frame off the
  LAN.
- **Consequence for existing setups:** two senders with the same MAC (two
  emulators with the same `--eth-mac` on one hub) no longer silently steal
  each other's traffic; the second is dropped while the first is up. A QEMU
  endpoint that never sends hellos and is idle for 15 s can be displaced.

### F45. ethhub: no rate limit, one broadcast amplified x63

- **Status:** fixed. Per-client token buckets `--rate FPS[,BPS]` (default
  10000 frames/s, 12.5 MB/s); a flood costs length x copies against
  `--flood-rate` (default 2 MB/s per client, the hub overall 4x), dropped whole
  when over budget. `--no-rate` and `--flood-rate 0` turn them off. Counters
  `dropped_rate`, `dropped_flood`.
- **Not verified:** the defaults against a real guest under load (the emulated
  DaynaPORT is far below them; no run came near them). Measured only with
  Python senders on loopback.

### F46. ethhub authentication (optional shared secret)

- **Status:** added, off by default. `--key-file FILE` (mode 0600, 16..1024
  bytes): a keyed 13-byte hello (`0xE7`, unix seconds, 8-byte truncated
  HMAC-SHA256) replaces the empty hello; only addresses that sent one may send
  frames; others are dropped (`dropped_unauth`). The emulator sends it with
  `--eth=hub:HOST:PORT,key=FILE` (or `MAXIVMAC_HUB_KEY_FILE`). The wire is
  unchanged without a key, and frames never change, so the QEMU dgram goal
  stands; a QEMU endpoint cannot join a keyed hub (it cannot send the hello),
  use `--allow` for those.
- **Threat model (see ETHHUB.md):** stops casual LAN abuse (join, read floods,
  inject, fill tables, reflect). Does not stop anyone who can sniff (replay of
  a hello for 60 s from any address, since NAT hides the client's address, and
  everything is in clear) or spoof a registered client's source address;
  one shared secret, no revocation; HMAC verification cost per 13-byte datagram.
  Per-datagram MACs, counters and nonces were left out on purpose.
- **Verified:** `ctest` (also under the ASan/UBSan preset), the Python tests,
  and the containerised e2e rung R1 (guest MacTCP ping through the hub and the
  container's tap) with a plain hub and with `E2E_HUB_KEY=1` (hub `--key-file`,
  emulator `,key=FILE`): both PASS, the keyed hub logs "authentication on" and
  `dropped_unauth=0`. R2/R3 were not run.
- **Not verified:** a hub and an emulator on two machines with skewed clocks;
  QEMU; a rate-limited or MAC-pinned guest under real load.
