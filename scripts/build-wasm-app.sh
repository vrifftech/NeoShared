#!/usr/bin/env bash
set -euo pipefail

SOURCE_ROOT=""
NEOSHARED_ROOT_VALUE=""
DEPS_ROOT=""
APP_TARGET=""
APP_NAME=""
SLUG=""
OPTION_PREFIX=""
CLI_OPTION=""
ICON=""
BUILD_DIR=""
DIST_DIR=""
JOBS=""
CLEAN=0

usage() {
  cat <<'USAGE'
usage: build-wasm-app.sh --source-root DIR --neoshared-root DIR --deps-root DIR --app-target TARGET --app-name NAME --slug SLUG --option-prefix PREFIX --icon FILE [options]

Options:
  --cli-option NAME  CMake option used to disable the native CLI
  --build-dir DIR    Default: <source>/build-wasm
  --dist-dir DIR     Default: <source>/dist-wasm
  --jobs N
  --clean
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-root) SOURCE_ROOT="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    --app-target) APP_TARGET="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --slug) SLUG="$2"; shift 2;;
    --option-prefix) OPTION_PREFIX="$2"; shift 2;;
    --cli-option) CLI_OPTION="$2"; shift 2;;
    --icon) ICON="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --dist-dir) DIST_DIR="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done
for value in SOURCE_ROOT NEOSHARED_ROOT_VALUE DEPS_ROOT APP_TARGET APP_NAME SLUG OPTION_PREFIX ICON; do
  [[ -n "${!value}" ]] || { echo "Missing required option: $value" >&2; exit 2; }
done
SOURCE_ROOT="$(cd "$SOURCE_ROOT" && pwd)"
NEOSHARED_ROOT_VALUE="$(cd "$NEOSHARED_ROOT_VALUE" && pwd)"
mkdir -p "$DEPS_ROOT"
DEPS_ROOT="$(cd "$DEPS_ROOT" && pwd)"
case "$ICON" in /*) ;; *) ICON="$SOURCE_ROOT/$ICON";; esac
[[ -f "$ICON" ]] || { echo "Icon was not found: $ICON" >&2; exit 2; }
[[ -n "$BUILD_DIR" ]] || BUILD_DIR="$SOURCE_ROOT/build-wasm"
[[ -n "$DIST_DIR" ]] || DIST_DIR="$SOURCE_ROOT/dist-wasm"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$SOURCE_ROOT/$BUILD_DIR";; esac
case "$DIST_DIR" in /*) ;; *) DIST_DIR="$SOURCE_ROOT/$DIST_DIR";; esac
[[ -n "$JOBS" ]] || JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

bash "$NEOSHARED_ROOT_VALUE/scripts/setup-wasm.sh" --deps-root "$DEPS_ROOT"
bash "$NEOSHARED_ROOT_VALUE/scripts/build-wx-wasm.sh" --deps-root "$DEPS_ROOT" --jobs "$JOBS"
# shellcheck disable=SC1090
source "$DEPS_ROOT/neo-wasm-versions.env"
# shellcheck disable=SC1090
source "$NEO_WASM_EMSDK_ROOT/emsdk_env.sh" >/dev/null
WX_BUILD="$DEPS_ROOT/wxwidgets-wasm-build"
[[ -x "$WX_BUILD/wx-config" ]] || { echo "wx-config was not produced" >&2; exit 1; }
[[ "$CLEAN" == 0 ]] || rm -rf "$BUILD_DIR"

cmake_args=(
  -S "$SOURCE_ROOT"
  -B "$BUILD_DIR"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_FLAGS=-fexceptions
  -DNEOSHARED_ROOT="$NEOSHARED_ROOT_VALUE"
  -DNEOSHARED_BUILD_TESTS=OFF
  -DNEO_MINIMAL_RELEASE=OFF
  -D"${OPTION_PREFIX}_BUILD_WX_GUI=ON"
  -D"${OPTION_PREFIX}_REQUIRE_WX_GUI=ON"
  -DwxWidgets_CONFIG_EXECUTABLE="$WX_BUILD/wx-config"
  -DwxWidgets_USE_STATIC=ON
  -DNEO_WX_WASM_SOURCE="$NEO_WASM_WX_SOURCE"
  -DNEO_WX_WASM_BUILD="$WX_BUILD"
  -DZLIB_INCLUDE_DIR="$(em-config CACHE)/sysroot/include"
  -DZLIB_LIBRARY="$(em-config CACHE)/sysroot/lib/wasm32-emscripten/libz.a"
)
[[ -z "$CLI_OPTION" ]] || cmake_args+=( -D"${CLI_OPTION}=OFF" )
embuilder build zlib
emcmake cmake "${cmake_args[@]}"
cmake --build "$BUILD_DIR" --target "$APP_TARGET" --parallel "$JOBS"

VERSION="$(sed -nE 's/^#define[[:space:]]+[A-Z0-9_]+_VERSION_STRING[[:space:]]+"([^"]+)".*/\1/p' "$SOURCE_ROOT/src/core/Version.hpp" | sed -n '1p')"
[[ -n "$VERSION" ]] || { echo "Unable to read application version" >&2; exit 1; }
bash "$NEOSHARED_ROOT_VALUE/scripts/package-wasm-site.sh" \
  --input "$BUILD_DIR/wasm-output" \
  --dist "$DIST_DIR" \
  --app-name "$APP_NAME" \
  --slug "$SLUG" \
  --version "$VERSION" \
  --icon "$ICON" \
  --source-root "$SOURCE_ROOT" \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  --wx-source "$NEO_WASM_WX_SOURCE"
echo "GitHub Pages site: $DIST_DIR"
