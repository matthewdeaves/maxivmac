#!/bin/bash
# Prepare the System 6.0.8 + DaynaPORT + MacTCP + MacTCP Ping test disk.
#
#   test/net/prep-disk.sh [OUTPUT.hfs]        (default $MAXIVMAC_NET_DISK)
#
# Inputs are environment variables, see lib.sh.  Exit 77 (skip) if anything is
# missing.  Nothing is written outside the cache/work directories and OUTPUT.
#
# Steps
#   1. Decode the Dayna installer to a floppy image (NDIF -> HFS, tools/netdisk/ndif.py).
#   2. Boot the base disk with that floppy in the emulator and run the Installer
#      (scripts/dayna-install.dbg): AppleTalk 58.0.0 for System 6 + DaynaPORT
#      SCSI/Link, then Restart.  This puts "Network", "AppleTalk",
#      "EtherTalk Phase 2" and "Responder" in the System Folder.
#   3. On the host, copy MacTCP to the volume ROOT (not into the System Folder!)
#      and MacTCP Ping into "Net Tools".
#
# ORDER MATTERS.  With MacTCP in the System Folder at boot, switching to EtherTalk
# in the Network control panel fails: "Could not switch to EtherTalk"; the
# guest's Open(".ATP") returns -108 memFullErr, the System 6 system heap having
# 27.9 KB free where EtherTalk needs about 31.4 KB (33 KB free without MacTCP).
# So the test (run-ping.sh) switches to EtherTalk first and only then drags
# MacTCP into the System Folder; this disk therefore carries MacTCP at the root.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
check_prep
# The disk is always prepared on a Mac Plus (the installer script is written for
# it); the prepared disk then runs on the Plus and the SE alike.
if [ "$MODEL" = MacPlus ]; then PREP_ROM="$ROM"; else PREP_ROM="${MAXIVMAC_PREP_ROM:-$REPO/data/roms/MacPlus.ROM}"; fi
[ -f "$PREP_ROM" ] || skip "Mac Plus ROM for preparing the disk not found: $PREP_ROM (set \$MAXIVMAC_PREP_ROM)"
OUT="${1:-$NET_DISK}"
mkdir -p "$CACHE" "$(dirname "$OUT")"
W="$(mktemp -d "${TMPDIR:-/tmp}/maxivmac-prep.XXXXXX")"
trap 'rm -rf "$W"' EXIT

# unpack_if_archive SRC NAME -> prints the path of the real file: SRC itself when
# it is not an archive, else the file called NAME that unar extracted.
unpack_if_archive() {
    local src="$1" name="$2" d
    if lsar "$src" >/dev/null 2>&1; then
        d="$W/unpack-$(echo "$name" | tr -c 'A-Za-z0-9' _)"
        mkdir -p "$d"
        unar -q -f -o "$d" "$src" >/dev/null || die "cannot unpack $src"
        local f
        f="$(find "$d" -type f -name "$name" | head -n 1)"
        [ -n "$f" ] || die "'$name' not found in $src"
        echo "$f"
    else
        echo "$src"
    fi
}

echo "== 1. Dayna installer floppy"
FLOPPY="$W/dayna.hfs"
src="$MAXIVMAC_DAYNA_INSTALLER"
case "$src" in
    *.hfs|*.dsk) cp "$src" "$FLOPPY" ;;
    *)
        if lsar "$src" >/dev/null 2>&1; then
            mkdir -p "$W/dayna-unpack"
            unar -q -f -o "$W/dayna-unpack" "$src" >/dev/null || die "cannot unpack $src"
            src="$(find "$W/dayna-unpack" -type f | head -n 1)"
            [ -n "$src" ] || die "nothing in the Dayna archive"
        fi
        case "$src" in
            *.hfs|*.dsk) cp "$src" "$FLOPPY" ;;
            *) python3 -I "$TOOLS/ndif.py" "$src" "$FLOPPY" ;;
        esac ;;
esac

echo "== 2. Run the Dayna installer in the emulator"
cp "$BASE_DISK" "$W/disk.hfs"
cp "$FLOPPY" "$W/dayna-floppy.hfs"
export MAXIVMAC_OUT_DIR="${MAXIVMAC_OUT_DIR:-$CACHE/prep-out}"
mkdir -p "$MAXIVMAC_OUT_DIR"
"$EMU" --headless --silent --model=MacPlus --rom="$PREP_ROM" \
    --dbg-script="$NETDIR/scripts/dayna-install.dbg" "$W/disk.hfs" "$W/dayna-floppy.hfs" \
    > "$MAXIVMAC_OUT_DIR/dayna-install.log" 2>&1 || die "Dayna installer run failed (see $MAXIVMAC_OUT_DIR/dayna-install.log)"
hmount "$W/disk.hfs" >/dev/null
hls ':System Folder' | grep -q "EtherTalk Phase 2" || { humount; die "Dayna install left no 'EtherTalk Phase 2' in the System Folder"; }
humount

echo "== 3. MacTCP (volume root) and MacTCP Ping (Net Tools)"
mactcp="$(unpack_if_archive "$MAXIVMAC_MACTCP" MacTCP)"
ping="$(unpack_if_archive "$MAXIVMAC_PING" "MacTCP Ping")"
python3 -I "$TOOLS/macbinary.py" "$mactcp" "$W/MacTCP.bin" >/dev/null
python3 -I "$TOOLS/macbinary.py" "$ping" "$W/MacTCP Ping.bin" >/dev/null
hmount "$W/disk.hfs" >/dev/null
hcopy -m "$W/MacTCP.bin" ':'
hmkdir ':Net Tools'
hcopy -m "$W/MacTCP Ping.bin" ':Net Tools:'
hls -l : ':Net Tools'
humount

cp "$W/disk.hfs" "$OUT"
echo "prepared: $OUT"
