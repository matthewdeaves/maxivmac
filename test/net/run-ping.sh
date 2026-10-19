#!/bin/bash
# MacTCP ping over the emulated DaynaPORT, end to end.
#
#   test/net/run-ping.sh [path/to/maxivmac]
#
# Prepares the test disk if needed (prep-disk.sh; cached in $MAXIVMAC_NET_CACHE),
# then, on a fresh copy of it, in ONE emulator session (the Network cdev's
# EtherTalk choice lives in PRAM, which is not kept between runs):
#   boot -> Chooser: AppleTalk Active -> Network cdev: EtherTalk -> drag MacTCP into
#   the System Folder -> Restart -> MacTCP cdev: Ethernet Built-In, manual
#   10.0.2.15/24, gateway 10.0.2.2 -> Restart -> MacTCP Ping 10.0.2.2
# Passes when the guest ran the whole script, the run saw five ICMP echo
# replies (SCSI trace) and the Ping window shows 5 packets out and 5 in.
#
# MAXIVMAC_ARGS: extra emulator arguments (e.g. --eth-latency=0 for the control run).
#
# Exit: 0 pass, 1 fail, 77 skip (an input is missing; see lib.sh).
# Evidence (screenshots, scsi trace, log) goes to $MAXIVMAC_OUT_DIR.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ $# -ge 1 ] && EMU="$1"
check_common
need_tool hls; need_tool hmount; need_tool humount
if [ ! -f "$NET_DISK" ]; then
    check_prep
    "$NETDIR/prep-disk.sh" "$NET_DISK" || exit $?
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/maxivmac-ping.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
export MAXIVMAC_OUT_DIR="${MAXIVMAC_OUT_DIR:-$CACHE/ping-out}"
mkdir -p "$MAXIVMAC_OUT_DIR"

# Assemble the script.  (`source` inside a script loses the lines after the
# first wait, see docs/FINDINGS.md, so the blocks are concatenated.)
BLOCKS="boot chooser cpopen ether move restart cpopen mactcp-open mactcp-config mactcp-close restart launch ping"
: > "$WORK/ping.dbg"
for b in $BLOCKS; do cat "$NETDIR/scripts/ping/$b.inc" >> "$WORK/ping.dbg"; done
cp "$WORK/ping.dbg" "$MAXIVMAC_OUT_DIR/ping.dbg"

cp "$NET_DISK" "$WORK/disk.hfs"
"$EMU" --headless --silent --model=MacPlus --rom="$ROM" \
    --scsi4=daynaport --eth=slirp --diag=scsi ${MAXIVMAC_ARGS:-} \
    --dbg-script="$WORK/ping.dbg" "$WORK/disk.hfs" \
    > "$MAXIVMAC_OUT_DIR/ping.out" 2> "$MAXIVMAC_OUT_DIR/ping.err"
rc=$?
echo "emulator exit $rc"
[ "$rc" -eq 0 ] || { echo "FAIL  script did not complete (exit $rc); see $MAXIVMAC_OUT_DIR/ping.out"; exit 1; }

# Evidence 1: echo replies seen by the adapter.  An echo request is a 98-byte
# frame the guest writes; a reply is READ data of 6 + 98 + 4 = 108 bytes.
replies=$(grep -c 'DATA-IN len 108 ' "$MAXIVMAC_OUT_DIR/ping.out" || true)
requests=$(grep -c 'DATA-OUT record 98 bytes' "$MAXIVMAC_OUT_DIR/ping.out" || true)
echo "echo requests written: $requests, 108-byte READ records: $replies"
# Evidence 2: what the Ping application drew.
if grep -q '5/5/0/0' "$MAXIVMAC_OUT_DIR/ping.out"; then tally=yes; else tally=no; fi
echo "Ping window tally 5/5/0/0 drawn: $tally"
if [ "$replies" -ge 5 ] && [ "$requests" -ge 5 ] && [ "$tally" = yes ]; then
    echo "PASS  MacTCP ping 10.0.2.2: 5 sent, 5 received"
    exit 0
fi
echo "FAIL  (see screenshots in $MAXIVMAC_OUT_DIR)"
exit 1
