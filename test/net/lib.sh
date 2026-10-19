# Shared by the test/net scripts (source it, do not run it).
#
# Every input comes from an environment variable; none of the inputs is part of
# the repository (Apple, Dayna and third-party software).  Missing input is a
# SKIP, not a failure: `skip` prints the reason and exits 77 (the ctest
# SKIP_RETURN_CODE).
#
#   MAXIVMAC            emulator binary        (default: bld/macos/maxivmac)
#   MAXIVMAC_ROM        Mac Plus ROM           (default: data/roms/MacPlus.ROM)
#   MAXIVMAC_BASE_DISK  bootable System 6.0.8 HFS image, the one the
#                       golden tests use       (default: extras/disks/608.hfs)
#   MAXIVMAC_DAYNA_INSTALLER  Dayna "DaynaPORT Installer" (the .sit, the NDIF
#                       .img inside it, or the HFS floppy decoded from it)
#   MAXIVMAC_MACTCP     MacTCP 2.0.x: the extracted "MacTCP" file or the
#                       archive (.sit/.bin) holding it
#   MAXIVMAC_PING       MacTCP Ping 2.0.x: the extracted "MacTCP Ping" or its archive
#   MAXIVMAC_NET_DISK   where the prepared disk is kept (default: under
#                       MAXIVMAC_NET_CACHE)
#   MAXIVMAC_NET_CACHE  cache directory (default: ${TMPDIR:-/tmp}/maxivmac-net)
#   MAXIVMAC_OUT_DIR    screenshots and traces of a run (default: under the cache)
#   MAXIVMAC_HFSUTILS   directory with hmount, hcopy, ... if not on PATH
#
# Host tools: hfsutils (hmount hcopy hls humount), unar/lsar (archives), python3,
# xattr (macOS: classic Mac forks live in extended attributes).

NETDIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO="$(cd "$NETDIR/../.." && pwd)"
TOOLS="$REPO/tools/netdisk"

skip() { echo "SKIP  $*"; exit 77; }
die() { echo "ERROR $*" >&2; exit 1; }

[ -n "${MAXIVMAC_HFSUTILS:-}" ] && PATH="$MAXIVMAC_HFSUTILS:$PATH"
EMU="${MAXIVMAC:-$REPO/bld/macos/maxivmac}"
ROM="${MAXIVMAC_ROM:-$REPO/data/roms/MacPlus.ROM}"
BASE_DISK="${MAXIVMAC_BASE_DISK:-$REPO/extras/disks/608.hfs}"
CACHE="${MAXIVMAC_NET_CACHE:-${TMPDIR:-/tmp}/maxivmac-net}"
NET_DISK="${MAXIVMAC_NET_DISK:-$CACHE/net-disk.hfs}"

need_tool() { command -v "$1" >/dev/null 2>&1 || skip "host tool '$1' not found${2:+ ($2)}"; }
need_file() { [ -f "$2" ] || skip "$1 not found${2:+: $2} (set \$$3)"; }

# check_inputs_for_prep / check_inputs_for_ping: skip unless everything is there.
check_common() {
    [ -x "$EMU" ] || die "emulator not found at $EMU (set MAXIVMAC)"
    need_file "Mac Plus ROM" "$ROM" MAXIVMAC_ROM
    need_tool python3
}
check_prep() {
    check_common
    need_tool hmount "hfsutils; set MAXIVMAC_HFSUTILS"
    need_tool hcopy; need_tool hls; need_tool humount; need_tool unar; need_tool xattr
    need_file "base System 6.0.8 disk" "$BASE_DISK" MAXIVMAC_BASE_DISK
    [ -n "${MAXIVMAC_DAYNA_INSTALLER:-}" ] && [ -f "$MAXIVMAC_DAYNA_INSTALLER" ] || skip "Dayna installer not found (set \$MAXIVMAC_DAYNA_INSTALLER)"
    [ -n "${MAXIVMAC_MACTCP:-}" ] && [ -f "$MAXIVMAC_MACTCP" ] || skip "MacTCP not found (set \$MAXIVMAC_MACTCP)"
    [ -n "${MAXIVMAC_PING:-}" ] && [ -f "$MAXIVMAC_PING" ] || skip "MacTCP Ping not found (set \$MAXIVMAC_PING)"
}
