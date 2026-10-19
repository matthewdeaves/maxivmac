# Static analysis

`tools/static-analysis.sh` runs clang-tidy, cppcheck, shellcheck, ruff and
lizard and compares the counts with `tools/static-analysis.baseline`. It is
read-only on the source tree; everything it writes (including a CMake build
dir for `compile_commands.json`) goes to `--out` (default
`$PWD/build/static-analysis`, which is git-ignored).

```sh
tools/static-analysis.sh --fast                 # clang-tidy on the newest files only, all other steps
tools/static-analysis.sh                        # everything (clang-tidy over the whole tree takes minutes)
tools/static-analysis.sh --steps "cppcheck lizard"
tools/static-analysis.sh --update-baseline      # after a deliberate change; never with --fast
```

Exit status: 0 fine, 1 a count went up (or cppcheck reports an `error`), 2 a
tool is missing. Counts may only go down: the baseline is a ratchet, so after
fixing findings run `--update-baseline` and commit the new file.

## Tools and versions

The baseline was recorded with clang-tidy 23 (Homebrew LLVM 23.1.3; the script
looks in `/opt/homebrew/opt/llvm/bin`), cppcheck 2.22, shellcheck 0.11, ruff
0.17 and lizard 1.24. A different version prints a warning (`SA_PIN_STRICT=1`
makes it an error); counts from a different version are not comparable.
`CLANG_TIDY`, `RUN_CLANG_TIDY`, `CPPCHECK`, `SHELLCHECK`, `RUFF`, `LIZARD`,
`CMAKE_ARGS` and `SA_JOBS` override binaries, CMake flags and parallelism.

## Configuration

| File | What |
|------|------|
| `.clang-tidy` | bugprone, clang-analyzer, cert, concurrency, performance, portability minus the classes measured to be noise (the reasons are in the file) |
| `src/.clang-tidy` | re-enables `cert-err33-c` for functions whose unchecked failure is a real bug (`fwrite`, `fclose`, `sendto`, `recv`, ...) |
| `src/cpu/.clang-tidy` | the 68k core is derived from Mini vMac and checked bit for bit by the goldens: mechanical style checks off, UB/analyzer checks on |
| `test/.clang-tidy` | doctest macros expand into unchecked calls |
| `tools/cppcheck.suppressions` | verified false positives and singleton rule-of-three noise, with reasons |
| `tools/static-analysis.baseline` | one `key value` line per metric (per-tool finding counts, lizard CCN/NLOC numbers) |

`libs/`, Objective-C++ files and the stb implementation are not analysed.
Lizard also reports the numbers for the files still being written
(`lizard.new.*`, the `NEWCODE_RE` pattern in the script).

## Related

Sanitizers: `cmake --preset macos-asan` instruments every target (the
emulator and the unit tests); run `ctest` and `test/run_script_tests.sh
<path-to-sanitized-maxivmac>` from it. What the first sweep found is in
`docs/FINDINGS.md` (F27 onward).
