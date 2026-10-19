#!/bin/sh
# Cross-compile maxivmac (and its unit tests) for Windows with mingw-w64 in a
# Debian container, mirroring the `windows` CMake preset. Catches compile and
# link errors before a real Windows CI run; it cannot run the resulting .exe's.
# Usage: tools/windows-precheck.sh [gcc|clang]   (default gcc)
set -eu
cc=${1:-gcc}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
docker build -t maxivmac-win-precheck "$here/windows-precheck"
case $cc in
  gcc)   tc=/tmp/mingw-toolchain.cmake ;;
  clang) tc=/tmp/mingw-clang-toolchain.cmake ;;
  *) echo "usage: $0 [gcc|clang]" >&2; exit 2 ;;
esac
docker run --rm -v "$src":/src:ro maxivmac-win-precheck sh -c "
  set -e
  cmake -S /src -B /tmp/bld -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$tc
  cmake --build /tmp/bld -- -k 0
  ls -l /tmp/bld/*.exe"
