#!/bin/sh
# Integration tests for the scripting system (headless .dbg scripts).
#
#   ./test/run_script_tests.sh [path/to/maxivmac]
#
# Environment:
#   MAXIVMAC_TEST_ROM   Mac Plus ROM   (default: data/roms/MacPlus.ROM)
#   MAXIVMAC_TEST_DISK  bootable System 6 HFS image
#                       (default: extras/disks/608.hfs, the verify.sh disk)
#   MAXIVMAC_OUT_DIR    scratch dir for screenshots (default: a temp dir)
#
# Tests that need the disk are SKIPped when it is absent; the disk is
# copied before use because the emulator writes to it.  Exit status is 1 if
# any test FAILs, 0 otherwise.
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR" || exit 1
EMU="${1:-$DIR/bld/macos/maxivmac}"
ROM="${MAXIVMAC_TEST_ROM:-$DIR/data/roms/MacPlus.ROM}"
DISK="${MAXIVMAC_TEST_DISK:-$DIR/extras/disks/608.hfs}"
SCRIPTS="$DIR/test/scripts"

[ -x "$EMU" ] || { echo "ERROR: emulator not found at $EMU"; exit 1; }
[ -f "$ROM" ] || { echo "SKIP all: ROM not found at $ROM"; exit 0; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/maxivmac-script-tests.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
MAXIVMAC_OUT_DIR="${MAXIVMAC_OUT_DIR:-$WORK/out}"
export MAXIVMAC_OUT_DIR
mkdir -p "$MAXIVMAC_OUT_DIR"

PASS=0
FAIL=0
SKIP=0

pass() { echo "PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL  $1: $2"; FAIL=$((FAIL + 1)); }
skip() { echo "SKIP  $1: $2"; SKIP=$((SKIP + 1)); }

# run <script> [with-disk]  -> sets $RC and $OUT (combined output)
run() {
    script="$1"
    args=""
    if [ "${2:-}" = disk ]; then
        cp "$DISK" "$WORK/disk.hfs"
        args="$WORK/disk.hfs"
    fi
    # Deliberately run from an unrelated directory: data/ lookup must not
    # depend on the current directory.
    case "$script" in /*) ;; *) script="$SCRIPTS/$script" ;; esac
    OUT="$(cd "$WORK" && "$EMU" --headless --silent --model=MacPlus --rom="$ROM" \
        --dbg-script="$script" $args 2>&1)"
    RC=$?
}

# run_many <script>...  -> several --dbg-script flags, in order, on the disk
run_many() {
    cp "$DISK" "$WORK/disk.hfs"
    flags=""
    for sc in "$@"; do flags="$flags --dbg-script=$sc"; done
    OUT="$(cd "$WORK" && "$EMU" --headless --silent --model=MacPlus --rom="$ROM" \
        $flags "$WORK/disk.hfs" 2>&1)"
    RC=$?
}

# expect <name> <script> <rc> [grep-pattern] [disk]
expect() {
    name="$1"; script="$2"; want="$3"; pat="${4:-}"; disk="${5:-}"
    if [ -n "$disk" ] && [ ! -f "$DISK" ]; then
        skip "$name" "disk $DISK not found (set MAXIVMAC_TEST_DISK)"
        return
    fi
    run "$script" $disk
    if [ "$RC" -ne "$want" ]; then
        fail "$name" "exit $RC, wanted $want"
        echo "$OUT" | tail -5
    elif [ -n "$pat" ] && ! echo "$OUT" | grep -q "$pat"; then
        fail "$name" "output lacks '$pat'"
        echo "$OUT" | tail -5
    else
        pass "$name"
    fi
}

# ── Needs only a ROM ───────────────────────────────────
expect "default-timeout 100ms is 0.100 s"  units_timeout.dbg     1 "expired after 0.100 s emulated"
expect "wait ... 50ms is 0.050 s"          explicit_timeout.dbg  1 "expired after 0.050 s emulated"
expect "unknown command exits 2"           bad_command.dbg       2 "FAIL"
expect "bad timeout exits 2"               bad_wait_arg.dbg      2 "cannot parse timeout"
expect "mouse commands parse and run"      mouse_cmds.dbg        0 "script completed"

run /nonexistent/script.dbg
if [ "$RC" -eq 2 ]; then pass "missing script exits 2"; else fail "missing script exits 2" "exit $RC"; fi

# Headless needs a model; without one it used to crash on a null machine.
OUT="$("$EMU" --headless --silent --rom="$ROM" 2>&1)"; RC=$?
if [ "$RC" -eq 2 ] && echo "$OUT" | grep -q -- "--model"; then pass "headless without --model exits 2 with a message"; else fail "headless without --model exits 2 with a message" "exit $RC"; fi

# ── Needs a bootable disk ──────────────────────────────
expect "boot to Finder"                    boot_wait.dbg         0 "text match" disk
expect "timeout exits non-zero"            must_timeout.dbg      1 "TIMEOUT" disk

expect "open File menu" menu_open.dbg 0 "script completed" disk
if [ -f "$MAXIVMAC_OUT_DIR/menu_open.png" ] && [ -f "$MAXIVMAC_OUT_DIR/menu_desktop.png" ]; then
    if cmp -s "$MAXIVMAC_OUT_DIR/menu_open.png" "$MAXIVMAC_OUT_DIR/menu_desktop.png"; then
        fail "menu screenshot differs from desktop" "images identical (menu did not open)"
    else
        pass "menu screenshot differs from desktop"
    fi
    # Determinism: an identical second run must give identical pixels.
    cp "$MAXIVMAC_OUT_DIR/menu_open.png" "$WORK/first_menu.png"
    run menu_open.dbg disk
    if cmp -s "$WORK/first_menu.png" "$MAXIVMAC_OUT_DIR/menu_open.png"; then
        pass "identical runs give identical screenshots"
    else
        fail "identical runs give identical screenshots" "screenshots differ"
    fi
fi

expect "key and type edit a new folder name" type_verify.dbg 0 "script completed" disk
expect "pick Apple menu item" menu_pick.dbg 0 "script completed" disk
expect "Restart boots from the same disk again" restart_keeps_disk.dbg 0 "script completed" disk

# --disk-snapshot: the guest may write, the image on the host must not change.
if [ -f "$DISK" ]; then
    cp "$DISK" "$WORK/snap.hfs"
    BEFORE="$(cksum < "$WORK/snap.hfs")"
    (cd "$WORK" && "$EMU" --headless --silent --model=MacPlus --rom="$ROM" --disk-snapshot \
        --dbg-script="$SCRIPTS/restart_keeps_disk.dbg" "$WORK/snap.hfs" >"$WORK/snap.out" 2>&1)
    SRC=$?
    if [ "$SRC" -eq 0 ] && grep -q "script completed" "$WORK/snap.out" \
        && [ "$BEFORE" = "$(cksum < "$WORK/snap.hfs")" ]; then
        pass "--disk-snapshot leaves the image unchanged"
    else
        fail "--disk-snapshot leaves the image unchanged" "exit $SRC or image modified"
    fi
else
    skip "--disk-snapshot leaves the image unchanged" "disk $DISK not found"
fi
expect "wait text recent sees text drawn before it" text_recent.dbg 0 "Text already drawn" disk
expect "plain wait text does not see older text"    text_not_recent.dbg 1 "TIMEOUT" disk
expect "clear text empties the recent buffer"       text_cleared.dbg    1 "TIMEOUT" disk
expect "recent honours its window"                  text_too_old.dbg    1 "TIMEOUT" disk

# `source` inside a script: a wait in the sourced file must not drop the lines
# after it, nor the lines after the `source` itself.
if [ -f "$DISK" ]; then
    rm -f "$MAXIVMAC_OUT_DIR"/src_*.png
    cat > "$WORK/source_outer.dbg" <<EOF
set default-timeout 120s
source "$SCRIPTS/source_inner.dbg"
screenshot "src_outer_after"
EOF
    run "$WORK/source_outer.dbg" disk
    if [ "$RC" -eq 0 ] && [ -f "$MAXIVMAC_OUT_DIR/src_inner_after_wait.png" ] \
        && [ -f "$MAXIVMAC_OUT_DIR/src_inner_end.png" ] && [ -f "$MAXIVMAC_OUT_DIR/src_outer_after.png" ]; then
        pass "source keeps the lines after a wait"
    else
        fail "source keeps the lines after a wait" "exit $RC; $(ls "$MAXIVMAC_OUT_DIR" | tr '\n' ' ')"
    fi

    rm -f "$MAXIVMAC_OUT_DIR"/multi_*.png
    run_many "$SCRIPTS/multi_a.dbg" "$SCRIPTS/multi_b.dbg"
    if [ "$RC" -eq 0 ] && [ -f "$MAXIVMAC_OUT_DIR/multi_a.png" ] && [ -f "$MAXIVMAC_OUT_DIR/multi_b.png" ]; then
        pass "several --dbg-script files run in order"
    else
        fail "several --dbg-script files run in order" "exit $RC"
    fi
else
    skip "source keeps the lines after a wait" "disk $DISK not found"
    skip "several --dbg-script files run in order" "disk $DISK not found"
fi

echo ""
echo "PASS: $PASS  FAIL: $FAIL  SKIP: $SKIP"
[ "$FAIL" -eq 0 ]
