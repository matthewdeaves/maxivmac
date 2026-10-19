#!/bin/bash
# Build PeerTalk's 68k MacTCP test apps (Retro68, out of tree) as MacBinary files.
#
#   test/net/build-peertalk-68k.sh OUTDIR [app ...]     (default apps: test_mesh test_chat)
#
# Environment (nothing is fetched, PeerTalk is only read):
#   PEERTALK_DIR     PeerTalk checkout (read-only input)           required
#   CLOG_DIR         clog sources                                   required
#   RETRO68_DIR      Retro68 toolchain prefix required (e.g. ~/Retro68-build/toolchain)
#   RETRO68_REZ      a working Rez, if the toolchain's own does not run (optional)
#
# Writes OUTDIR/<app>.bin (MacBinary, both forks) for add-apps.sh.  Exit 77 = skip.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
OUT="${1:?usage: build-peertalk-68k.sh OUTDIR [app ...]}"; shift || true
APPS=("$@"); [ ${#APPS[@]} -gt 0 ] || APPS=(test_mesh test_chat)
[ -n "${RETRO68_DIR:-}" ] || die "RETRO68_DIR is not set: point it at the Retro68 toolchain prefix (the directory holding m68k-apple-macos/)"
R68="$RETRO68_DIR"
[ -n "${PEERTALK_DIR:-}" ] && [ -f "$PEERTALK_DIR/CMakeLists.txt" ] || skip "PeerTalk checkout not found (set \$PEERTALK_DIR)"
[ -n "${CLOG_DIR:-}" ] && [ -f "$CLOG_DIR/CMakeLists.txt" ] || skip "clog not found (set \$CLOG_DIR)"
TC="$R68/m68k-apple-macos/cmake/retro68.toolchain.cmake"
[ -f "$TC" ] || skip "Retro68 toolchain not found at $R68 (set \$RETRO68_DIR)"
need_tool cmake
mkdir -p "$OUT"
B="$OUT/build"; mkdir -p "$B"
{ echo "include(\"$TC\")"; [ -n "${RETRO68_REZ:-}" ] && echo "set(REZ \"$RETRO68_REZ\")"; } > "$B/toolchain.cmake"
cmake -S "$PEERTALK_DIR" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$B/toolchain.cmake" \
    -DPT_PLATFORM=MACTCP -DCLOG_DIR="$CLOG_DIR" -DCMAKE_BUILD_TYPE=Release >"$B/cfg.log" 2>&1 || { cat "$B/cfg.log"; die "configure failed"; }
cmake --build "$B" --target "${APPS[@]/%/_APPL}" >"$B/make.log" 2>&1 || { tail -30 "$B/make.log"; die "build failed"; }
for a in "${APPS[@]}"; do
    [ -f "$B/$a.bin" ] || die "$a.bin not produced (Rez failing? set RETRO68_REZ); see $B/make.log"
    cp "$B/$a.bin" "$OUT/$a.bin"
    echo "built $OUT/$a.bin ($(wc -c <"$OUT/$a.bin") bytes)"
done
