#!/bin/bash
# Add MacBinary (.bin) applications to a prepared test disk, into ":Net Tools:".
#
#   test/net/add-apps.sh BASE.hfs OUT.hfs APP.bin [APP.bin ...]
#
# BASE.hfs is the disk prep-disk.sh made (it is not modified); OUT.hfs gets the
# apps.  hfsutils copies both forks from MacBinary.  Used by test/e2e for the
# PeerTalk 68k MacTCP test apps (build-peertalk-68k.sh) and BomberTalk.
# Exit 77 when a tool or file is missing.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ $# -ge 3 ] || die "usage: add-apps.sh BASE.hfs OUT.hfs APP.bin [APP.bin ...]"
BASE="$1"; OUT="$2"; shift 2
need_tool hmount "hfsutils; set MAXIVMAC_HFSUTILS"; need_tool hcopy; need_tool humount; need_tool hls
need_file "base disk" "$BASE" BASE
for a in "$@"; do need_file "application" "$a" APP; done
tmp="$(mktemp "${TMPDIR:-/tmp}/maxivmac-apps.XXXXXX")"
trap 'rm -f "$tmp"; humount >/dev/null 2>&1 || true' EXIT
cp "$BASE" "$tmp"
hmount "$tmp" >/dev/null
for a in "$@"; do hcopy -m "$a" ':Net Tools:'; done
hls -l ':Net Tools'
humount
cp "$tmp" "$OUT"
echo "disk with apps: $OUT"
