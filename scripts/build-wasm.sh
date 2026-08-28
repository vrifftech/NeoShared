#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_ROOT="${NEO_WASM_DEPS_ROOT:-$ROOT_DIR/../.neo-wasm-deps}"
BUILD_DIR="$ROOT_DIR/build-wasm"
BUILD_TYPE="Release"
JOBS=""
CLEAN=0
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
usage: ./scripts/build-wasm.sh [options] [-- extra-cmake-args...]

Options:
  --deps-root DIR    Emscripten dependency cache [default: ../.neo-wasm-deps]
  --build-dir DIR    Build directory [default: ./build-wasm]
  --build-type TYPE  CMake build type [default: Release]
  --jobs N           Parallel build jobs
  --clean            Remove the build directory before configuring
  --cmake-arg ARG    Additional CMake argument (repeatable)
  -h, --help         Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --jobs|--parallel) JOBS="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    --cmake-arg) EXTRA_CMAKE_ARGS+=("$2"); shift 2;;
    --) shift; EXTRA_CMAKE_ARGS+=("$@"); break;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

case "$DEPS_ROOT" in /*) ;; *) DEPS_ROOT="$ROOT_DIR/$DEPS_ROOT";; esac
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR";; esac
[[ -n "$JOBS" ]] || JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

if [[ "$CLEAN" == 1 ]]; then
  case "$BUILD_DIR" in
    "$ROOT_DIR"/*) rm -rf -- "$BUILD_DIR";;
    *) echo "Refusing to clean a build directory outside the repository: $BUILD_DIR" >&2; exit 2;;
  esac
fi

bash "$ROOT_DIR/scripts/setup-wasm.sh" --deps-root "$DEPS_ROOT" --skip-wx
# shellcheck disable=SC1090
source "$DEPS_ROOT/neo-wasm-versions.env"
# shellcheck disable=SC1090
source "$NEO_WASM_EMSDK_ROOT/emsdk_env.sh" >/dev/null

for command_name in emcmake cmake ninja; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable after activating emsdk: $command_name" >&2
    exit 2
  }
done

emcmake cmake \
  -S "$ROOT_DIR" \
  -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DNEO_MINIMAL_RELEASE=OFF \
  "${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
