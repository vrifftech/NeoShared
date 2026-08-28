#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
DEPLOYMENT_TARGET="15.0"
BUILD_DIR="$ROOT_DIR/build-macos"
BUILD_TYPE="Release"
JOBS=""
CLEAN=0

usage() {
  cat <<'USAGE'
usage: ./scripts/build-macos.sh [options]

Options:
  --arch arm64|x86_64       Native architecture [default: uname -m]
  --deployment-target VER   Requested minimum macOS version [default: 15.0]
  --build-dir DIR           Build directory [default: build-macos]
  --build-type TYPE         Build type [default: Release]
  --jobs N                  Parallel build jobs
  --clean                   Remove the build directory first
  -h, --help                Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    --deployment-target) DEPLOYMENT_TARGET="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

[[ "$(uname -s)" == "Darwin" ]] || { echo "This script must run on macOS." >&2; exit 2; }
case "$ARCH" in arm64|x86_64) ;; *) echo "Unsupported architecture: $ARCH" >&2; exit 2;; esac
[[ "$(uname -m)" == "$ARCH" ]] || { echo "Use a native $ARCH Mac or runner." >&2; exit 2; }
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR";; esac
[[ -n "$JOBS" ]] || JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf 2)"
export MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"

clean_args=()
[[ "$CLEAN" == 0 ]] || clean_args+=(--clean)
bash "$ROOT_DIR/scripts/build.sh" \
  --build-dir "$BUILD_DIR" \
  --build-type "$BUILD_TYPE" \
  --minimal-release OFF \
  --generator Ninja \
  --jobs "$JOBS" \
  "${clean_args[@]+"${clean_args[@]}"}" \
  -- \
  "-DCMAKE_OSX_ARCHITECTURES=$ARCH" \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET"
