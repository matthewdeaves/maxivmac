# Scripting

New debugger commands that let you drive the guest Mac — type text,
press keys, wait for things to appear on screen, take screenshots,
launch apps, and shut down.  These compose freely with existing
debugger commands (breakpoints, tracing, memory inspection), so you
can instrument a scripted run.

Commands that inject guest-side actions (`launch`, `exittoshell`,
`shutdown`) require the SharedDrive INIT to be loaded in the guest.

## Core model

Scripting is built on two existing debugger concepts:

1. **Breakpoints** — extended with new condition types (text match,
   screen match, power-off).
2. **Script suspension** — when a script hits `wait` or `continue`,
   execution suspends; the remaining lines resume automatically when
   the debugger next stops.

`wait` is sugar for: create a temporary breakpoint + continue.  When
the breakpoint fires, the temp BP is deleted and the script resumes
at the next line.  This means `wait` composes with all existing
breakpoint features (conditions, ignore counts, attached commands).

## Invocation

Scripts are regular `.dbg` files, run with existing flags:

```sh
maxivmac -disk boot.hfs --headless --dbg-script=build.dbg          # headless, max speed
maxivmac -disk boot.hfs --debugger --dbg-script=build.dbg          # visible window, drops to prompt on finish
```

`--headless` runs without a window at maximum speed (a cold System 6 boot
to the Finder takes ~2.5 s of wall time).  Without `--debugger`, the
script is run in *unattended script mode*, where the exit status tells
you what happened:

| Status | Meaning |
|---|---|
| 0 | script ran to the end (`[script completed]`) or executed `exit 0` |
| 1 | a `wait` timed out, `fail` ran, `exit 1`, or a screenshot could not be saved |
| 2 | the script itself is wrong: unreadable script file, unknown or ambiguous command, unparsable argument (e.g. a bad timeout, unresolvable key, unknown trap), a `commands` block |

The failing line is printed and, for timeouts and `fail`, a screenshot
is saved (see [screenshot](#screenshot)).

The run is deterministic: with the same ROM, disk image, and script, two
runs produce byte-identical screenshots and CPU traces (verified by
`test/run_script_tests.sh`).  Input timing is in emulated time, so
wall-clock speed does not matter.  A fixed `wait for` is still a guess
about how busy the guest is (the Finder is sometimes slow for a few
seconds after boot); synchronise on something the guest draws instead,
e.g. `mousedown 52 9` then `wait text "Eject"`.

Since it's just a `.dbg` file, you can also `source build.dbg` from
the interactive debugger prompt.

`--dbg-script` may be given several times; the files run one after another
as a single script, in the order given (a `wait` in the first holds the
second back).  A file that cannot be read stops the run with status 2 before
anything executes.

### source

```
source "other.dbg"
```

Runs the file's lines in place: they execute next, before the lines that
follow the `source` line, and a `wait` inside the file suspends the whole
script as usual.  The path is relative to the emulator's **current working
directory** (not the script's), and must be quoted when it contains `/`
(the tokenizer reads an unquoted `/` as an operator).

## New breakpoint types

The existing `break` command gains new condition types.  All
breakpoint features work with these: `commands`, `ignore`, `disable`,
`enable`, `delete`, conditional expressions.

### break text

```
break text "pattern"
```

Fire when the guest *draws* text containing the substring.  This is an
**event** match, not a screen reader: the emulator watches the A-line
traps that render text and compares the strings they are given.  It
sees:

| Trap | Text source |
|---|---|
| `DrawString` | Pascal string (menu titles and items, window titles, Finder icon names, most UI labels) |
| `DrawText`, `StdText`, `TextBox` | text buffer (`start`/`count` honoured), e.g. dialog static text, TextEdit, alert bodies |
| `ParamText` | the four substitution strings of an alert/dialog (when they are set) |

Strings are converted from MacRoman to UTF-8; the pattern is a plain
case-sensitive substring.  Run with `showtext on` to see exactly what is
captured and discover patterns.

Consequences worth knowing:

- Only text drawn **after** the `wait text` is armed can match.  The Finder
  menu bar is drawn once at boot; `wait text "File"` issued after the
  desktop is up will not see it, but `wait text "Special"` issued before
  boot completes will.  To check for something already on screen use
  `wait screen`, or pull the menu down again (`mousedown`) so it is
  redrawn.
- The match fires while the guest is still drawing.  The framebuffer shown
  by `screenshot` is updated at the end of each 60 Hz tick, so add a short
  `wait for 100ms` before a screenshot that should contain the text.
- Text rendered by other means (bitmap fonts blitted by an application,
  QuickDraw `CopyBits` of pre-rendered text, PICTs) is invisible to it.
- Needs `data/debug/traps.def` (found automatically, see BUILDING.md).
  With the definitions missing the emulator prints a warning at start-up.

Adding another trap is a one-line `traps.def` edit: give a Pascal-string
parameter the type `Text`, or a buffer/start/count triple the types
`TextBuf`/`TextStart`/`TextCount`.

### break screen

```
break screen "reference.png"
break screen "reference.png" <percent>
```

Fire when the framebuffer matches a reference PNG.  Checked once per
tick (60 Hz).

Match is by percentage of identical pixels.  Default threshold is
99.85% — just enough to ignore a 16×16 cursor anywhere on a 512×342
screen.  Override with a second argument: `break screen "desktop.png"
95` requires 95% of pixels to match.

The reference PNG must be at the same bit depth as the emulated
screen — comparison is raw pixel values, not color-matched.

Future: use the PNG alpha channel as a mask — only non-transparent
pixels are compared.  This lets you match a region of the screen
(e.g. a dialog box) regardless of what's behind it.

### break off

```
break off
```

Fire when the machine powers off (Mac II and later).  On machines
without power-off hardware, this breakpoint will never fire.

## Time units

Every duration in a script (`wait` timeouts, `wait for`, `timeout`,
`set default-timeout`) is measured in **scheduler units** of emulated time,
not wall-clock time.  One unit is 1/64 of a 68000 clock cycle
(`kCycleScale` = 64), which is about 1/1000 of an instruction.  A Mac
Plus/SE/Classic (7.8336 MHz) runs 501,350,400 units per emulated second;
a Mac II (clockMult 2) runs twice that.  A cold boot of System 6.0.8 to the
Finder on a Plus takes about 11.4e9 units (23 s emulated, ~2.5 s of wall
time when headless).

A bare number is a raw unit count.  Suffixes (written directly after the
number, `20s` or `20 s`) make durations readable:

| Suffix | Meaning |
|---|---|
| `s` | emulated seconds (converted with the running machine's clock) |
| `ms` | emulated milliseconds |
| `M`, `k` | x 1,000,000 / x 1,000 **raw units** (not cycles) |
| `c` | raw units (explicit) |
| `off`, `none` | no timeout (only valid where a timeout is expected) |

Prefer `s`/`ms`; the `M`/`k` forms are small amounts of emulated time
(1M = 2 ms on a Plus).

The default timeout is 60 s emulated (`set default-timeout`).  Timeouts
are checked once per 60 Hz tick, so the granularity is ~17 ms.

## The `wait` command

```
wait <condition> [timeout]
```

Create a temporary breakpoint with the given condition, then continue
execution.  When the breakpoint fires, it's deleted and the script
resumes at the next line.  If the timeout expires first, the command
fails.

`wait` accepts the same conditions as `break`, plus any existing
breakpoint target:

| Form | What it waits for |
|---|---|
| `wait text "pattern"` | Text match |
| `wait text "pattern" 20s` | Text match with explicit timeout |
| `wait screen "ref.png"` | Screen match |
| `wait screen "ref.png" 20s 95` | Screen match with timeout + threshold (percent) |
| `wait for 2s` | Run for a fixed amount of emulated time (never fails) |
| `wait off` | Power-off |
| `wait $40122E` | PC hits address |
| `wait main` | PC hits symbol |
| `wait trap GetResource` | Trap fires |

**Timeout:** Each `wait` carries a deadline (in emulated time, see
[Time units](#time-units)).  Whichever fires first (condition or timeout)
wins.  The timeout defaults to the value set by `timeout` /
`set default-timeout`; `wait ... off` waits forever.

**On timeout failure:**
- Print `TIMEOUT: wait (bp N) expired after ...` to the debugger output
- Save `timeout-screenshot.png` in the current working directory
- In unattended script mode (`--dbg-script` without `--debugger`): exit
  with status 1
- With `--debugger` / interactive: drop to the debugger prompt

**Interactive use:** If you type `wait` at the prompt with no pending
script, it behaves like `tbreak <condition>` + `continue` — stops
when the condition fires and shows the prompt.

## Other new commands

### timeout

```
timeout <duration>
set default-timeout <duration>
```

Set the default timeout for `wait` commands that don't specify their own.
Takes `5s`, `500ms`, `1M`, `off` or a raw unit count (see
[Time units](#time-units)).  Default: 60 s emulated.

### type

```
type "hello world"
```

Inject text as keystrokes.  UTF-8 in the script, converted to MacRoman
key events in the guest.  Each character produces a key-down/key-up
pair.  Does not resume the guest — just enqueues input.

### key

```
key return
key cmd-K
key cmd-shift-S
```

Inject a single key press, optionally with modifiers.  Does not resume
the guest — just enqueues a key-down/key-up pair into the input queue.

**Modifiers:** `cmd`, `shift`, `opt`, `ctrl` — combined with `-`.

**Special keys:** `return`, `enter`, `tab`, `escape`, `delete`, `space`,
`left`, `right`, `up`, `down`, `f1`–`f15`.

#### Key timing

Events are queued in scheduler units (see [Time units](#time-units)):
a key is held for 80,000 units (~0.16 ms emulated) and the next key
follows 160,000 units after the previous one's press; `key` presses
modifiers 10,000 units before the key and releases them 100,000 units
after the press.  Guest software sees every transition as its own keyboard
event, which is what typing needs (verified with `type` into a Finder
rename field), but a program that *polls* the keyboard state instead of
using events can miss such short presses.  If the queue is empty the first
event is immediate; subsequent events are scheduled after the last queued
key event.

`type` and `key` both push to the same queue — you can interleave
them freely, and the timing is cumulative.

### Mouse: click, drag, move, mousedown, mouseup

```
click 120 80                    # quick click
click 52 9 hold 1s              # press, hold 1 s, release
click button "OK"               # centre of a dialog button
drag 22 9 30 40                 # press at (22,9), glide to (30,40), release
drag 22 9 30 40 hold 1s         # ... dwell 1 s on the target before releasing
click 22 9 30 40                # same as drag
move 200 100                    # just move the pointer
mousedown 52 9                  # press (optionally after moving)
mouseup
```

Coordinates are Mac screen pixels (0,0 = top-left of the 512x342 Plus
screen).  Like `type` and `key`, mouse commands only **queue** events; the
guest runs when the script next waits, so follow them with `wait for` or
`wait text`.  Gestures queue behind each other (a `mouseup` after a
`drag` is delivered after the drag completes).

**Timing.**  The emulated mouse is sampled once per 60 Hz tick: one event
per tick, and after a button event the device ignores input for 4 ticks.
The gestures space their events in whole ticks to respect that:

| Gesture | Timeline |
|---|---|
| `click x y` | legacy: move, press, release; the device stretches the press to ~4 ticks. Fine for buttons, too quick for pull-down menus |
| `click x y hold D` | move, press 2 ticks later, release after `D` (at least 5 ticks) |
| `drag x1 y1 x2 y2` | move, press, wait 6 ticks, 10 glide steps 0.1 s apart, dwell 0.3 s (`hold D` overrides; min 8 ticks), release |
| `mousedown [x y]`, `mouseup`, `move x y` | one event each, queued after earlier gestures |

To open a menu and look at it: `mousedown 52 9`, `wait for 1s`,
`screenshot "menu"`, `mouseup`.  To choose an item, use `drag` from the
menu title to the item.

### info keys

```
info keys
```

Show the pending input queue — future key-down/key-up events and
their scheduled cycle counts.  Useful for debugging when keystrokes
aren't being consumed as expected.

### clearkeys

```
clearkeys
```

Discard all pending key events from the input queue.

### showtext

```
showtext on
showtext off
showtext
```

Toggle live display of captured text to the console.  When enabled,
every `Text`-typed trap parameter is printed as it fires:

```
[text] DrawString: "Open"
[text] ParamText: "Save changes to \"MyFile\"?"
```

Useful for discovering what strings to match with `wait text` or
`break text`.  With no argument, toggles.

### launch

```
launch "Shared Drive:My App"
```

Launch an application by its Mac path.  The INIT handles this via
the host→guest command channel — the emulator tells the INIT to
call `_Launch` with the given path.

The INIT's jGNE filter runs in the context of the current app, so
`_Launch` is valid — it terminates the current app and starts the
new one.  Under MultiFinder, `_Launch` should spawn a new process
instead of replacing the current one, but this needs testing.

### screenshot

```
screenshot "label"
```

Save the current framebuffer as `label.png` (the `.png` is added if the
name has no extension).  A relative name is written under
`$MAXIVMAC_OUT_DIR` when that environment variable is set (the directory
is created), otherwise relative to the **current working directory** of
the emulator process, not the script's directory.  An unwritable path
exits with status 1 in unattended mode.

On `fail` and on a `wait` timeout, `fail-screenshot.png` /
`timeout-screenshot.png` are saved the same way.

The framebuffer is updated once per 60 Hz tick, so a screenshot taken
in the same instant a `wait text` fires may not show that text yet; put
`wait for 100ms` first.

### exittoshell

```
exittoshell
```

Queue an `_ExitToShell` command to the INIT, terminating the current
application.  Under non-MultiFinder this returns to the Finder (or
whatever launched the app).

### shutdown

```
shutdown
```

Queue a `_ShutDown` command to the INIT — the Mac goes through its
normal shutdown sequence.  This is guest-side shutdown, not emulator
termination.

The script must wait for shutdown to complete:
- Mac Plus/SE: displays "You may now switch off your Macintosh
  safely" — use `wait text` to detect it.
- Mac II: powers off — use `wait off`.

### fail

```
fail "something went wrong"
```

Terminate the emulator with exit code 1 and print the message to
stderr.  Useful in `commands` blocks on breakpoints to catch error
conditions early.

## Script suspension and resumption

When a script (`source` or `--dbg-script`) encounters a command that
resumes the CPU (`wait`, `continue`, `run`), execution **suspends**:
the remaining lines are saved.  When the debugger next stops (any
breakpoint, timeout, or manual interrupt), the script **resumes**
from the next line.

This means scripts are linear sequences of commands interspersed with
wait points.  No nesting, no recursion, no re-entrancy.

**What happens at a user breakpoint during a `wait`:**

If you have `break GetResource` active and a `wait text "Open"` is
running, the address breakpoint fires first.  The script is still
suspended.  You get the interactive prompt.  When you type `continue`,
the CPU resumes and the `wait text` timeout keeps ticking.  When the
text breakpoint eventually fires (or times out), the script resumes.

This means user breakpoints compose: they interrupt execution, you
inspect, you continue, and the script picks up where it left off.

## Composability with existing commands

Because `break text` and `break screen` are regular breakpoints, all
existing features work.

> **Not yet working in script files:** the `commands ... end` blocks in
> the examples below read their command list from the interactive
> console, not from the `.dbg` file.  In unattended script mode a
> `commands` line is rejected with exit status 2 rather than silently
> running the following lines immediately.  Use them from the interactive
> prompt (`--debugger`) for now.

```
# Persistent text breakpoint with auto-commands (≈ "ontext")
break text "Replace existing"
  commands
    key return
  end

# Catch errors — persistent, never deleted
break text "Error"
  commands
    screenshot "error"
    fail "compile error"
  end

# Trace every instruction between two UI events
wait text "Open"
trace insns on
key return
wait text "MyFile.c"
trace insns off

# Break on a trap during a compile
wait text "SharedDrive.c"
break GetResource
key cmd-K
wait text "Build succeeded" 50s
delete 1
```

Note: persistent breakpoints with `commands` that include `key` or
`continue` give you reactive handlers — the old `ontext` concept
falls out for free.

## Example: build an INIT with THINK C

```
# build.dbg — Build SharedDrive INIT with THINK C
# Boot disk: thinkc-build.hfs (THINK C as startup app)
# SharedDrive: shared/ (contains project + source)

timeout 5s

# Dismiss unexpected "Replace?" dialogs automatically
break text "Replace existing"
  commands
    key return
    continue
  end

# THINK C launches, shows Open dialog
wait text "Open"
key tab                            # switch to SharedDrive volume
type "Shared Drive"
key return

# Project opens, source window visible
wait text "SharedDrive.c"

# Build
key cmd-K
wait text "Save code resource" 50s   # long compile
key return

# Back to editor = build complete
wait text "SharedDrive.c"
shutdown
```
