#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] -- [cmake options]

Options:
  -b DIR      Build directory (default: ${BUILD_DIR})
  -j N        Parallel jobs for build (default: ${JOBS})
  -c TYPE     CMake build type (Release, Debug). If omitted, CMake default is used.
  -h          Show this help

Any arguments after '--' are forwarded to 'cmake -S . -B <builddir>'.
Examples:
  $(basename "$0") -b build -j4 -- -DCMAKE_BUILD_TYPE=Release
  BUILD_DIR=out ./scripts/build.sh
EOF
}

while getopts ":b:j:c:h" opt; do
  case $opt in
    b) BUILD_DIR="$OPTARG" ;;
    j) JOBS="$OPTARG" ;;
    c) CMAKE_BUILD_TYPE="$OPTARG" ;;
    h) usage; exit 0 ;;
    \?) echo "Unknown option: -$OPTARG" >&2; usage; exit 2 ;;
  esac
done
shift $((OPTIND-1))

CM_ARGS=("-S" "$ROOT_DIR" "-B" "$BUILD_DIR")
[ -n "${CMAKE_BUILD_TYPE:-}" ] && CM_ARGS+=("-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

# forward any extra args
if [ "$#" -gt 0 ]; then
  CM_ARGS+=("$@")
fi

echo "[build] root: $ROOT_DIR"
echo "[build] build dir: $BUILD_DIR"
echo "[build] cmake args: ${CM_ARGS[*]}"

# configure
cmake "${CM_ARGS[@]}"

# build using cmake --build so generator is respected
echo "[build] building (-j $JOBS)"
cmake --build "$BUILD_DIR" -- -j "$JOBS"

echo "[build] finished. Artifacts in: $BUILD_DIR"
