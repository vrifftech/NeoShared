#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_BIN="${CMAKE:-cmake}"
BUILD_DIR="$ROOT_DIR/build"
BUILD_TYPE="Release"
BUILD_TESTS="ON"
MINIMAL_RELEASE="OFF"
JOBS="${JOBS:-}"
TARGET=""
GENERATOR=""
CLEAN=0
EXTRA=()

usage() {
  cat <<USAGE
usage: ./scripts/build.sh [options] [-- extra-cmake-args...]

Options:
  --build-dir DIR          Build directory [default: ./build]
  --build-type TYPE        Debug, Release, RelWithDebInfo, or MinSizeRel
  --tests ON|OFF           Build regression tests [default: ON]
  --minimal-release ON|OFF Enable release-minimization flags [default: OFF]
  --jobs N                 Parallel build jobs
  --target NAME            Build a specific CMake target
  --generator NAME         CMake generator name
  --clean                  Delete the build directory before configuring
  -h, --help               Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --tests) BUILD_TESTS="$2"; shift 2;;
    --minimal-release) MINIMAL_RELEASE="$2"; shift 2;;
    --jobs|--parallel) JOBS="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --generator) GENERATOR="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; EXTRA+=("$@"); break;;
    *) EXTRA+=("$1"); shift;;
  esac
done

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
fi
if [[ "$CLEAN" == 1 && -e "$BUILD_DIR" ]]; then
  case "$BUILD_DIR" in
    "$ROOT_DIR"/*) rm -rf -- "$BUILD_DIR";;
    *) echo "Refusing to clean a build directory outside the repository: $BUILD_DIR" >&2; exit 2;;
  esac
fi
mkdir -p "$BUILD_DIR"

CONFIG_ARGS=(-S "$ROOT_DIR" -B "$BUILD_DIR"
  "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
  "-DNEOSHARED_BUILD_TESTS=$BUILD_TESTS"
  "-DNEO_MINIMAL_RELEASE=$MINIMAL_RELEASE")
if [[ -n "$GENERATOR" ]]; then
  CONFIG_ARGS=(-G "$GENERATOR" "${CONFIG_ARGS[@]}")
fi
CONFIG_ARGS+=("${EXTRA[@]}")

"$CMAKE_BIN" "${CONFIG_ARGS[@]}"
BUILD_ARGS=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
[[ -z "$JOBS" || "$JOBS" == 0 ]] || BUILD_ARGS+=(--parallel "$JOBS")
[[ -z "$TARGET" ]] || BUILD_ARGS+=(--target "$TARGET")
"$CMAKE_BIN" "${BUILD_ARGS[@]}"
