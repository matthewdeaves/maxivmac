#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
#
# Repeatable static analysis: clang-tidy, cppcheck, shellcheck, ruff, lizard.
#
#   tools/static-analysis.sh [--out DIR] [--steps "tidy cppcheck shell ruff lizard"]
#                            [--update-baseline] [--fast]
#
# Read-only on the source tree.  Everything it writes goes to --out
# (default: $PWD/build/static-analysis), including the CMake build dir used for
# compile_commands.json.  Intended to run unchanged on a developer Mac and in a
# Linux container with the tools pinned (see "Pinned versions" below).
#
# Gating is a RATCHET: tools/static-analysis.baseline holds one "key value" line
# per metric (finding counts per tool and the lizard numbers of
# docs/QUALITY_FRAMEWORK_V2_PROPOSAL.md M2).  A run fails (exit 1) when a count
# goes UP; --update-baseline rewrites the file after a deliberate change.
# Exit: 0 ok, 1 ratchet violated / hard findings, 2 tool missing or wrong version.
#
# Environment overrides (all optional):
#   CMAKE_ARGS       extra cmake -D flags for the compile_commands build
#   CLANG_TIDY RUN_CLANG_TIDY CPPCHECK SHELLCHECK RUFF LIZARD   tool binaries
#   SA_JOBS          parallelism (default: number of CPUs)
#   SA_PIN_STRICT=1  fail (exit 2) when a tool version differs from the pin
#   SA_BASELINE      baseline file to ratchet against / rewrite (default tools/static-analysis.baseline, the pinned-toolchain
#                    one).  CI containers whose distro tools differ from the pins use their own file
#                    (tools/static-analysis.baseline.debian13); the Mac gate never reads it.
#
# Pinned versions (what the baseline was recorded with; bump together with it):
PIN_CLANG_TIDY="23"        # major; Homebrew LLVM 23.1.3
PIN_CPPCHECK="2.22"
PIN_SHELLCHECK="0.11"
PIN_RUFF="0.17"
PIN_LIZARD="1.24"

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${PWD}/build/static-analysis"
STEPS="tidy cppcheck shell ruff lizard"
UPDATE=0
FAST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --steps) STEPS="$2"; shift 2 ;;
        --update-baseline) UPDATE=1; shift ;;
        --fast) FAST=1; shift ;;   # tidy: only the new-code files (ncr5380, scsi_*, eth_*, slirp, cmd_script, ethhub)
        -h|--help) sed -n 2,28p "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
JOBS="${SA_JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}"
BASELINE="${SA_BASELINE:-$ROOT/tools/static-analysis.baseline}"
CURRENT="$OUT/metrics.txt"
: > "$CURRENT"
FAILED=0

# ---------------------------------------------------------------- helpers
find_tool() { # find_tool VAR default [extra search dirs...]
    local var="$1" def="$2"; shift 2
    local cur="${!var:-}"
    [ -n "$cur" ] && { echo "$cur"; return; }
    if command -v "$def" >/dev/null 2>&1; then command -v "$def"; return; fi
    for d in "$@"; do [ -x "$d/$def" ] && { echo "$d/$def"; return; }; done
    echo ""
}
need() { # need NAME PATH PIN VERSION_STRING
    local name="$1" path="$2" pin="$3" ver="$4"
    if [ -z "$path" ]; then echo "ERROR: $name not found" >&2; exit 2; fi
    echo "tool  $name = $path  ($ver)" | tee -a "$OUT/versions.txt"
    case "$ver" in *"$pin"*) ;; *)
        echo "WARN: $name version '$ver' differs from pin '$pin'" >&2
        [ "${SA_PIN_STRICT:-0}" = 1 ] && exit 2 ;;
    esac
}
metric() { echo "$1 $2" >> "$CURRENT"; }   # metric KEY VALUE
want() { case " $STEPS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

: > "$OUT/versions.txt"

# New-code file name pattern (lizard split, clang-tidy --fast).
NEWCODE_RE='(ncr5380|scsi_|eth_|slirp|cmd_script|script_|duration|startup_disks|ethhub)'

# ------------------------------------------------------ compile_commands.json
make_cdb() {
    local bd="$OUT/build"
    if [ ! -f "$bd/compile_commands.json" ]; then
        echo "== cmake (compile_commands.json) in $bd"
        # shellcheck disable=SC2086
        cmake -S "$ROOT" -B "$bd" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${CMAKE_ARGS:-} >"$OUT/cmake.log" 2>&1 \
            || { echo "cmake failed, see $OUT/cmake.log" >&2; exit 2; }
        # driver43_data.h is generated: build just the generated headers.
        ninja -C "$bd" imgutil >>"$OUT/cmake.log" 2>&1 || true
    fi
    # Filtered database: no libs/, no Objective-C++, one entry per file.
    python3 -I - "$bd/compile_commands.json" "$OUT/cdb" "$FAST" "$NEWCODE_RE" <<'PY'
import json, os, re, sys
src, outdir, fast, rx = sys.argv[1:5]
os.makedirs(outdir, exist_ok=True)
seen, out = set(), []
for e in json.load(open(src)):
    f = e["file"]
    if "/libs/" in f or f.endswith(".mm") or "stb_impl" in f or f in seen:
        continue
    if fast == "1" and not re.search(rx, os.path.basename(f)):
        continue
    seen.add(f); out.append(e)
json.dump(out, open(os.path.join(outdir, "compile_commands.json"), "w"), indent=1)
print(f"compile_commands: {len(out)} translation units")
PY
}

# ------------------------------------------------------------------ clang-tidy
step_tidy() {
    local tidy rct
    tidy="$(find_tool CLANG_TIDY clang-tidy /opt/homebrew/opt/llvm/bin /usr/lib/llvm-23/bin)"
    rct="$(find_tool RUN_CLANG_TIDY run-clang-tidy /opt/homebrew/opt/llvm/bin /usr/lib/llvm-23/bin)"
    need clang-tidy "$tidy" "$PIN_CLANG_TIDY" "$("$tidy" --version 2>/dev/null | grep -i version | head -1)"
    make_cdb
    echo "== clang-tidy (config: $ROOT/.clang-tidy)"
    # The checks and per-directory exceptions live in .clang-tidy files, not here.
    ( cd "$ROOT" && "$rct" -clang-tidy-binary "$tidy" -p "$OUT/cdb" -j "$JOBS" -quiet \
        -header-filter="$ROOT/(src|tools|test)/.*" \
        "$ROOT/(src|tools|test)/.*\\.(cpp|c)\$" ) >"$OUT/clang-tidy.txt" 2>&1 || true
    # Unique findings (headers are reported once per TU): file:line:col check
    grep -E '^/.*: (warning|error): .*\[[a-zA-Z0-9.,_-]+\]$' "$OUT/clang-tidy.txt" \
        | sed -E "s|^$ROOT/||; s|^([^:]+:[0-9]+:[0-9]+): (warning\|error): .*\[([^]]+)\]\$|\1 \3|" \
        | sort -u >"$OUT/clang-tidy.findings"
    local total; total="$(wc -l <"$OUT/clang-tidy.findings" | tr -d ' ')"
    metric tidy_total "$total"
    # Per-check counts (ratchet each one, so a new check class cannot hide in the total).
    awk '{ n=split($2, a, ","); c[a[1]]++ } END { for (k in c) print "tidy." k, c[k] }' \
        "$OUT/clang-tidy.findings" | sort >>"$CURRENT"
    echo "clang-tidy: $total unique findings (see $OUT/clang-tidy.findings)"
}

# --------------------------------------------------------------------- cppcheck
step_cppcheck() {
    local cc; cc="$(find_tool CPPCHECK cppcheck /opt/homebrew/bin)"
    need cppcheck "$cc" "$PIN_CPPCHECK" "$("$cc" --version 2>/dev/null)"
    make_cdb
    echo "== cppcheck"
    local extra=()
    [ -f "$ROOT/tools/cppcheck.suppressions" ] && extra+=(--suppressions-list="$ROOT/tools/cppcheck.suppressions")
    "$cc" --project="$OUT/cdb/compile_commands.json" --enable=warning,performance,portability \
        --inconclusive --inline-suppr -j "$JOBS" -q \
        --suppress=missingIncludeSystem --suppress='*:*/Applications/*' --suppress='*:/usr/*' \
        --suppress='*:*/opt/homebrew/*' --suppress='*:*/libs/*' "${extra[@]}" \
        --template='{file}:{line}:{column}: {severity}:{id}: {message}' 2>"$OUT/cppcheck.txt" || true
    grep -E '^[^ ]+:[0-9]+:[0-9]+: (error|warning|performance|portability)' "$OUT/cppcheck.txt" \
        | sed -E "s|^$ROOT/||" | sort -u >"$OUT/cppcheck.findings"
    metric cppcheck_total "$(wc -l <"$OUT/cppcheck.findings" | tr -d ' ')"
    # cppcheck "error" severity is a hard gate (no baseline): real defects only.
    local errs; errs="$(grep -c ': error:' "$OUT/cppcheck.findings" || true)"
    metric cppcheck_errors "$errs"
    echo "cppcheck: $(wc -l <"$OUT/cppcheck.findings" | tr -d ' ') findings, $errs errors"
}

# ------------------------------------------------------------------ shellcheck
step_shell() {
    local sc; sc="$(find_tool SHELLCHECK shellcheck /opt/homebrew/bin)"
    need shellcheck "$sc" "$PIN_SHELLCHECK" "$("$sc" --version | sed -n 's/^version: //p')"
    echo "== shellcheck"
    local files
    files="$(cd "$ROOT" && find . \( -path ./libs -o -path ./macdocs -o -path ./macsrc -o -path ./foo \
              -o -path ./build -o -path ./bld -o -path ./.git \) -prune -o -type f -name '*.sh' -print | sort)"
    # shellcheck disable=SC2086
    ( cd "$ROOT" && "$sc" -x -f gcc -S style $files ) >"$OUT/shellcheck.txt" 2>&1 || true
    grep -E '^[^ ]+:[0-9]+:[0-9]+: ' "$OUT/shellcheck.txt" | sort -u >"$OUT/shellcheck.findings" || true
    metric shellcheck_total "$(wc -l <"$OUT/shellcheck.findings" | tr -d ' ')"
    metric shellcheck_warn_or_error "$(grep -cE ': (warning|error):' "$OUT/shellcheck.findings" || true)"
    echo "shellcheck: $(wc -l <"$OUT/shellcheck.findings" | tr -d ' ') findings"
}

# ------------------------------------------------------------------------ ruff
step_ruff() {
    local r; r="$(find_tool RUFF ruff /opt/homebrew/bin)"
    need ruff "$r" "$PIN_RUFF" "$("$r" --version)"
    echo "== ruff"
    local py
    py="$(cd "$ROOT" && find tools docker test -name '*.py' -not -path '*/libs/*' | sort)"
    # Default rule set plus the bug-finding families; style families (UP, SIM, EXE, I) stay off.
    # shellcheck disable=SC2086
    ( cd "$ROOT" && "$r" check --no-cache --select E9,F,B,S,PLE,PLW --ignore S101,S603,S607,S104,S311 \
        --output-format concise $py ) >"$OUT/ruff.txt" 2>&1 || true
    grep -E '^[^ ]+:[0-9]+:[0-9]+: ' "$OUT/ruff.txt" | sort -u >"$OUT/ruff.findings" || true
    metric ruff_total "$(wc -l <"$OUT/ruff.findings" | tr -d ' ')"
    echo "ruff: $(wc -l <"$OUT/ruff.findings" | tr -d ' ') findings"
}

# ---------------------------------------------------------------------- lizard
# Same measurements as QUALITY_FRAMEWORK_V2 M2 (CCN>15, CCN>25, NLOC>100, averages)
# for the framework scope (src/ + tools/ethhub), plus the same counts restricted
# to the files that are still being written (NEWCODE_RE).
step_lizard() {
    local lz; lz="$(find_tool LIZARD lizard "$HOME/.local/bin")"
    need lizard "$lz" "$PIN_LIZARD" "$("$lz" --version)"
    echo "== lizard"
    ( cd "$ROOT" && "$lz" src tools/ethhub -l cpp -l c -x '*/libs/*' -x '*/stb_*' --csv ) >"$OUT/lizard.csv" 2>"$OUT/lizard.err"
    python3 -I - "$OUT/lizard.csv" "$NEWCODE_RE" "$CURRENT" <<'PY'
import csv, re, sys
rows = [r for r in csv.reader(open(sys.argv[1])) if len(r) >= 11]
rx, cur = re.compile(sys.argv[2]), open(sys.argv[3], "a")
fn = [(int(r[0]), int(r[1]), r[6], r[7], r[9]) for r in rows]   # nloc, ccn, file, name, line
def emit(prefix, sel):
    n = len(sel) or 1
    cur.write(f"{prefix}.functions {len(sel)}\n")
    cur.write(f"{prefix}.avg_ccn_x100 {round(100*sum(f[1] for f in sel)/n)}\n")
    cur.write(f"{prefix}.ccn_gt15 {sum(f[1] > 15 for f in sel)}\n")
    cur.write(f"{prefix}.ccn_gt25 {sum(f[1] > 25 for f in sel)}\n")
    cur.write(f"{prefix}.nloc_gt100 {sum(f[0] > 100 for f in sel)}\n")
emit("lizard.all", fn)
new = [f for f in fn if rx.search(f[2])]
emit("lizard.new", new)
print("worst new-code functions (CCN):")
for f in sorted(new, key=lambda f: -f[1])[:8]:
    print(f"  CCN {f[1]:3d} NLOC {f[0]:4d}  {f[3]}  {f[2]}:{f[4]}")
PY
}

# ------------------------------------------------------------------------ run
want tidy     && step_tidy
want cppcheck && step_cppcheck
want shell    && step_shell
want ruff     && step_ruff
want lizard   && step_lizard

# ------------------------------------------------------------- ratchet / report
# Counts may only go down.  "avg_ccn_x100" etc. are plain integers.
if [ "$UPDATE" = 1 ]; then
    sort -u "$CURRENT" >"$BASELINE"
    echo "baseline written: $BASELINE"
    exit 0
fi
if [ -f "$BASELINE" ]; then
    echo "== ratchet vs $BASELINE"
    while read -r key val; do
        base="$(awk -v k="$key" '$1 == k { print $2 }' "$BASELINE")"
        [ -z "$base" ] && { echo "  new   $key = $val (not in baseline)"; continue; }
        if [ "$val" -gt "$base" ]; then
            echo "  WORSE $key: $base -> $val"; FAILED=1
        elif [ "$val" -lt "$base" ]; then
            echo "  better $key: $base -> $val (run --update-baseline to lock it in)"
        fi
    done <"$CURRENT"
else
    echo "no baseline ($BASELINE): metrics only, nothing gated"
fi
# Hard gate independent of the baseline: cppcheck "error" severity.
if grep -qE '^cppcheck_errors [1-9]' "$CURRENT" 2>/dev/null; then
    base="$(awk '$1=="cppcheck_errors"{print $2}' "$BASELINE" 2>/dev/null || echo 0)"
    cur="$(awk '$1=="cppcheck_errors"{print $2}' "$CURRENT")"
    [ "$cur" -gt "${base:-0}" ] && FAILED=1
fi
echo "reports: $OUT"
exit "$FAILED"
