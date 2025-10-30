#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--distclean]

Options:
  --distclean   Remove the entire build directory instead of running the clean target.
  -h            Show this help
EOF
}

DISTCLEAN=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --distclean) DISTCLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1"; usage; exit 2 ;;
  esac
done

if [ "$DISTCLEAN" -eq 1 ]; then
  echo "[clean] removing build dir: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
  echo "[clean] done"
  exit 0
fi

if [ ! -d "$BUILD_DIR" ]; then
  echo "[clean] build dir does not exist: $BUILD_DIR"
  exit 0
fi

echo "[clean] running 'cmake --build <builddir> --target clean'"
cmake --build "$BUILD_DIR" --target clean || {
  echo "[clean] cmake clean failed, falling back to rm -rf"
  rm -rf "$BUILD_DIR"
}

echo "[clean] done"
