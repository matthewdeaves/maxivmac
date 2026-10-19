#!/bin/bash
# Make the guest disk for test/e2e/run.sh (native: needs macOS hfsutils/unar and Retro68).
#
#   test/e2e/make-disk.sh [OUT.hfs]       (default $E2E_DISK, else ./e2e-guest.hfs)
#
# 1. test/net/prep-disk.sh      System 6.0.8 + DaynaPORT + MacTCP + MacTCP Ping (cached)
# 2. test/net/build-peertalk-68k.sh   PeerTalk's test_mesh and test_chat (68k MacTCP)
# 3. test/net/add-apps.sh       those, and BomberTalk, into ":Net Tools:"
#
# The Finder lays the four applications out in name order, which is what the click
# coordinates of test/e2e/in-container/e2e.py assume (ICON_X): BomberTalk, MacTCP Ping,
# test_chat, test_mesh.  Without BOMBERTALK_68K the disk has no BomberTalk icon and
# the others shift one place left: pass it, or R4 and the others will click the wrong icon.
#
# Environment: everything prep-disk.sh needs (MAXIVMAC, MAXIVMAC_ROM, MAXIVMAC_DAYNA_INSTALLER,
# MAXIVMAC_MACTCP, MAXIVMAC_PING, MAXIVMAC_HFSUTILS, ...; see docs/features/NETWORKING_TESTING.md), plus
#   PEERTALK_DIR, CLOG_DIR, RETRO68_DIR/RETRO68_REZ   see test/net/build-peertalk-68k.sh
#   BOMBERTALK_68K   BomberTalk.bin (MacBinary) built for 68k (required, see above)
# Exit 77 = skip (an input is missing).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NET="$HERE/../net"
. "$NET/lib.sh"
OUT="${1:-${E2E_DISK:-$PWD/e2e-guest.hfs}}"
[ -n "${BOMBERTALK_68K:-}" ] && [ -f "$BOMBERTALK_68K" ] || skip "BomberTalk 68k MacBinary not found (set \$BOMBERTALK_68K)"
mkdir -p "$CACHE"
[ -f "$NET_DISK" ] || "$NET/prep-disk.sh" "$NET_DISK" || exit $?   # reused when it exists (delete it to rebuild)
APPS="$CACHE/e2e-apps"
"$NET/build-peertalk-68k.sh" "$APPS" test_mesh test_chat || exit $?
"$NET/add-apps.sh" "$NET_DISK" "$OUT" "$APPS/test_mesh.bin" "$APPS/test_chat.bin" "$BOMBERTALK_68K"
