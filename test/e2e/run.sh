#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
#
# Host-side orchestration of the containerised end-to-end test (the ONLY native
# part: it drives Docker; everything else runs inside the container).
# See docs/features/NETWORKING_E2E.md.
#
#   test/e2e/run.sh [RUNG ...]            default rungs: r1 r2 r3
#
# Inputs (environment; none is in the repository, none is baked into the image):
#   MAXIVMAC_ROM   Mac Plus ROM                       (default data/roms/MacPlus.ROM, else extras/roms/vMac.ROM)
#   E2E_DISK       the prepared guest disk: test/net/prep-disk.sh, then
#                  test/net/add-apps.sh with the PeerTalk 68k apps (test_mesh, test_chat)
#   PEERTALK_DIR   PeerTalk checkout (read-only; POSIX peer is built from it)
#   CLOG_DIR       clog sources (read-only; nothing is fetched)
#   BOMBERTALK_DIR optional BomberTalk checkout (read-only), built as the POSIX peer of R4 and R5
#   PEERTALK_DIR_OLD  optional second PeerTalk checkout (pre-fix), built as /opt/peers/old;
#                  select it for a run with E2E_PEER=old
# Options:
#   E2E_OUT        output directory (default ./e2e-out/<timestamp>)
#   E2E_IMAGE      use this image instead of building one
#   E2E_PEER       new (default) | old        which POSIX PeerTalk build the peer is
#   E2E_SPEED      emulator --speed N (default: the emulator's own default, 4 = 16x; the
#                  scripts' fixed waits assume it: at --speed=0 the guest is too slow for them)
#   E2E_CPUS E2E_MEMORY E2E_PIDS E2E_TIMEOUT (per-container hard limit, seconds)
#   E2E_REPEAT     run each rung N times (default 1); results in <out>/<rung>-<n>/
#   E2E_EXPECT_FAIL=1  negative control (e.g. E2E_PEER=old r3): the run is expected to FAIL;
#                  it is reported XFAIL and exits 0, a pass is XPASS and exits 1
#   MAXIVMAC_HFSUTILS  directory with hmount/hcopy (host-side PT_Log cross-check)
#
# Exit: 0 all rungs pass, 1 a rung failed, 77 skipped (an input or docker is missing).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
skip() { echo "SKIP  $*"; exit 77; }
die() { echo "ERROR $*" >&2; exit 1; }
[ -n "${MAXIVMAC_HFSUTILS:-}" ] && PATH="$MAXIVMAC_HFSUTILS:$PATH"

ROM="${MAXIVMAC_ROM:-}"
[ -z "$ROM" ] && for c in "$REPO/data/roms/MacPlus.ROM" "$REPO/extras/roms/vMac.ROM"; do [ -f "$c" ] && ROM="$c" && break; done
[ -n "$ROM" ] && [ -f "$ROM" ] || skip "Mac Plus ROM not found (set \$MAXIVMAC_ROM)"
[ -n "${E2E_DISK:-}" ] && [ -f "$E2E_DISK" ] || skip "guest disk not found (set \$E2E_DISK; see docs/features/NETWORKING_E2E.md)"
command -v docker >/dev/null 2>&1 || skip "docker not found"
docker info >/dev/null 2>&1 || skip "docker daemon not reachable"
RUNGS=("$@"); [ ${#RUNGS[@]} -gt 0 ] || RUNGS=(r1 r2 r3)
NEEDS_IMAGE=1; [ -n "${E2E_IMAGE:-}" ] && NEEDS_IMAGE=0
if [ $NEEDS_IMAGE = 1 ]; then
    [ -n "${PEERTALK_DIR:-}" ] && [ -f "$PEERTALK_DIR/CMakeLists.txt" ] || skip "PeerTalk checkout not found (set \$PEERTALK_DIR)"
    [ -n "${CLOG_DIR:-}" ] && [ -f "$CLOG_DIR/CMakeLists.txt" ] || skip "clog not found (set \$CLOG_DIR)"
    command -v rsync >/dev/null 2>&1 || skip "rsync not found"
fi

OUT="${E2E_OUT:-$PWD/e2e-out/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT" && chmod 777 "$OUT" || die "cannot create $OUT"
# Staging lives under the output directory, not $TMPDIR: with Colima/Docker Desktop only
# some host paths (the home directory) are visible to the Docker VM, and bind mounts of
# anything else silently turn into empty directories.
TMPROOT="$OUT/.stage"; rm -rf "$TMPROOT"; mkdir -p "$TMPROOT"
trap 'rm -rf "$TMPROOT"; [ -n "${STATS_PID:-}" ] && kill "$STATS_PID" 2>/dev/null' EXIT

# ---- the image: build context is a staged, filtered copy -----------------------
if [ $NEEDS_IMAGE = 1 ]; then
    CTX="$TMPROOT/ctx"; mkdir -p "$CTX/repo" "$CTX/e2e" "$CTX/peertalk" "$CTX/peertalk-old" "$CTX/bombertalk" "$CTX/clog"
    rsync -a --exclude .git --exclude bld --exclude docs --exclude media --exclude macdocs \
          --exclude macsrc --exclude foo --exclude extras --exclude '*.hfs' --exclude e2e-out \
          --exclude /docker --exclude /test/net --exclude /test/e2e "$REPO/" "$CTX/repo/"
    rsync -a --relative "$REPO/./docker/e2e" "$REPO/./test/e2e" "$REPO/./test/net" "$CTX/e2e/"
    rsync -a --exclude .git --exclude build "$PEERTALK_DIR/" "$CTX/peertalk/"
    [ -n "${PEERTALK_DIR_OLD:-}" ] && rsync -a --exclude .git --exclude build "$PEERTALK_DIR_OLD/" "$CTX/peertalk-old/"
    [ -n "${BOMBERTALK_DIR:-}" ] && rsync -a --exclude .git --exclude books --exclude docs --exclude notes \
          --exclude specs --exclude '/build*' "$BOMBERTALK_DIR/" "$CTX/bombertalk/"
    rsync -a --exclude .git "$CLOG_DIR/" "$CTX/clog/"
    cp "$REPO/docker/e2e/.dockerignore" "$CTX/.dockerignore"
    HASH="$(cd "$CTX" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum | shasum | cut -c1-8)"
    E2E_IMAGE="maxivmac-e2e:$(cat "$REPO/docker/e2e/VERSION")-$HASH"
    if docker image inspect "$E2E_IMAGE" >/dev/null 2>&1; then
        echo "image $E2E_IMAGE present"
    else
        echo "building $E2E_IMAGE (context $(du -sh "$CTX" | cut -f1)) ..."
        docker build -q -t "$E2E_IMAGE" -f "$CTX/e2e/docker/e2e/Dockerfile" "$CTX" >"$OUT/image-build.log" 2>&1 \
            || { tail -30 "$OUT/image-build.log"; die "image build failed (see $OUT/image-build.log)"; }
    fi
fi
docker image inspect "$E2E_IMAGE" >/dev/null 2>&1 || die "image $E2E_IMAGE not found"
echo "image: $E2E_IMAGE ($(docker image inspect -f '{{.Size}}' "$E2E_IMAGE" | awk '{printf "%.0f MB", $1/1e6}'))" | tee "$OUT/image.txt"

# ---- inputs, staged world-readable (the container runs as uid 2010) --------------
IN="$TMPROOT/in"; mkdir -p "$IN/rom" "$IN/disk"
cp "$ROM" "$IN/rom/vMac.ROM"
cp -c "$E2E_DISK" "$IN/disk/guest.hfs" 2>/dev/null || cp "$E2E_DISK" "$IN/disk/guest.hfs"
chmod -R a+rX "$IN"

CPUS="${E2E_CPUS:-2}"; MEM="${E2E_MEMORY:-1g}"; PIDS="${E2E_PIDS:-256}"; TMO="${E2E_TIMEOUT:-1500}"
REPEAT="${E2E_REPEAT:-1}"
rc_all=0

# One container per rung run.  Flags (docs/features/NETWORKING_E2E.md has the rationale):
#   --network none        no network but the container's own tap0 + loopback
#   --cap-drop ALL, then NET_ADMIN (create tap0), SETUID/SETGID (drop to uid 2010);
#                         the entrypoint drops every capability before the emulator starts
#   --device /dev/net/tun for the tap; --read-only root, tmpfs /tmp and /work
docker_run() {  # docker_run NAME RUNG OUTDIR
    docker run --rm --name "$1" \
        --network none \
        --cap-drop ALL --cap-add NET_ADMIN --cap-add SETUID --cap-add SETGID \
        --device /dev/net/tun \
        --security-opt no-new-privileges \
        --read-only \
        --tmpfs /tmp:rw,noexec,nosuid,size=64m \
        --tmpfs /work:rw,nosuid,size=768m,uid=2010,gid=2010,mode=0700 \
        --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" --pids-limit "$PIDS" \
        --log-driver json-file --log-opt max-size=10m --log-opt max-file=3 \
        -v "$IN:/in:ro" -v "$3:/out" \
        -e E2E_FLAT=1 -e E2E_TIMEOUT="$TMO" -e E2E_PEER="${E2E_PEER:-new}" -e E2E_SPEED="${E2E_SPEED:-}" \
        "$E2E_IMAGE" "$2"
}

json_note() {  # json_note FILE KEY VALUE : append VALUE to the list under KEY in summary.json
    python3 - "$1" "$2" "$3" <<'PY'
import json, sys
p, k, v = sys.argv[1:4]
s = json.load(open(p)); s.setdefault(k, []).append(v)
json.dump(s, open(p, "w"), indent=2)
PY
}

crosscheck_ptlog() {  # crosscheck_ptlog RUNDIR : read PT_Log back from the disk the run left (hfsutils)
    local d="$1" gz="$1/disk-after.hfs.gz" ok=false detail
    [ -f "$gz" ] || return 0
    [ -f "$d/summary.json" ] || return 0
    grep -q '"rung": "r\(2\|2b\|3\|3b\)"' "$d/summary.json" || return 0   # only the test_mesh rungs write PT_test_mesh
    if ! { command -v hmount >/dev/null 2>&1 && command -v hcopy >/dev/null 2>&1; }; then
        json_note "$d/summary.json" not_verified "PT_Log cross-check: hfsutils (hmount/hcopy) not on the host (set MAXIVMAC_HFSUTILS)"
        return 0
    fi
    local img="$TMPROOT/after.hfs"
    gunzip -c "$gz" > "$img"
    hmount "$img" >/dev/null 2>&1
    hcopy -t ':Net Tools:PT_test_mesh' "$d/PT_test_mesh.txt" >/dev/null 2>&1 \
        || hcopy -t ':PT_test_mesh' "$d/PT_test_mesh.txt" >/dev/null 2>&1
    humount >/dev/null 2>&1
    rm -f "$img"
    if [ -s "$d/PT_test_mesh.txt" ]; then
        # hfsutils text mode already maps CR to LF
        if grep -q '\[CONN\]' "$d/PT_test_mesh.txt" && grep -q 'PASS' "$d/PT_test_mesh.txt"; then
            ok=true; detail="PT_test_mesh read from the disk with hfsutils: $(grep -c . "$d/PT_test_mesh.txt") lines, has [CONN] and PASS"
        else
            detail="PT_test_mesh read, but no [CONN]/PASS in it (see PT_test_mesh.txt)"
        fi
    else
        detail="PT_test_mesh not found on the disk after the run"
    fi
    python3 - "$d/summary.json" "$ok" "$detail" <<'PY'
import json, sys
p, ok, d = sys.argv[1:4]
s = json.load(open(p))
s["checks"]["ptlog_crosscheck"] = {"ok": ok == "true", "detail": d}
if ok != "true":
    s["status"] = "FAIL"
json.dump(s, open(p, "w"), indent=2)
PY
}

start=$(date +%s)
for rung in "${RUNGS[@]}"; do
    n=1
    while [ "$n" -le "$REPEAT" ]; do
        label="$rung"; [ "$REPEAT" -gt 1 ] && label="$rung-$n"
        rdir="$OUT/$label"; mkdir -p "$rdir"; chmod 777 "$rdir"
        name="maxivmac-e2e-$rung-$$-$n"
        echo "== $label: docker run ($CPUS cpus, $MEM, $PIDS pids, timeout ${TMO}s) -> $rdir"
        (
            while sleep 20; do
                docker stats --no-stream --format '{{.CPUPerc}} {{.MemUsage}} {{.PIDs}}' "$name" 2>/dev/null \
                    | sed "s/^/$(date +%H:%M:%S) /" >> "$rdir/docker-stats.log"
            done
        ) & STATS_PID=$!
        # host-side backstop in case the in-container timeout does not fire
        ( sleep $((TMO + 120)); docker kill "$name" >/dev/null 2>&1 ) & WD=$!
        t0=$(date +%s)
        docker_run "$name" "$rung" "$rdir" > "$rdir/container.log" 2>&1
        rc=$?
        kill $WD $STATS_PID 2>/dev/null; wait $WD $STATS_PID 2>/dev/null; STATS_PID=
        echo "   container exit $rc after $(( $(date +%s) - t0 )) s"
        # the driver wrote everything for this run straight into $rdir (/out)
        crosscheck_ptlog "$rdir"
        if [ -f "$rdir/summary.json" ]; then
            st="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "$rdir/summary.json")"
        else
            st=FAIL
        fi
        [ "$rc" -eq 0 ] || st=FAIL
        if [ "${E2E_EXPECT_FAIL:-0}" = 1 ]; then
            if [ "$st" = FAIL ]; then st=XFAIL; else st=XPASS; fi
            python3 - "$rdir/summary.json" "$st" <<'PY'
import json, sys
p, st = sys.argv[1:3]
s = json.load(open(p)); s["status"] = st; s["expected_failure"] = True
json.dump(s, open(p, "w"), indent=2)
PY
            [ "$st" = XFAIL ] || rc_all=1
        else
            [ "$st" = PASS ] || rc_all=1
        fi
        echo "   $label: $st"
        echo "$label $st" >> "$OUT/results.txt"
        n=$((n + 1))
    done
done
python3 - "$OUT" "$E2E_IMAGE" "$(( $(date +%s) - start ))" <<'EOF'
import glob, json, os, sys
out, image, secs = sys.argv[1:4]
runs = {}
for p in sorted(glob.glob(os.path.join(out, "*", "summary.json"))):
    s = json.load(open(p))
    runs[os.path.basename(os.path.dirname(p))] = {"status": s["status"], "wall_s": s.get("wall_s"),
        "failed_checks": [k for k, v in s["checks"].items() if not v["ok"]]}
json.dump({"image": image, "wall_s": int(secs), "runs": runs,
           "status": "PASS" if runs and all(r["status"] in ("PASS", "XFAIL") for r in runs.values()) else "FAIL"},
          open(os.path.join(out, "summary.json"), "w"), indent=2)
EOF
cat "$OUT/summary.json"
exit $rc_all
