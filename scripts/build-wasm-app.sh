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
VERSION=""
BUILD_DIR=""
DIST_DIR=""
JOBS=""
CLEAN=0
VCPKG_ROOT_VALUE=""
VCPKG_TRIPLET="wasm32-emscripten"
VCPKG_INSTALLED_DIR=""
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
usage: build-wasm-app.sh --source-root DIR --neoshared-root DIR --deps-root DIR --app-target TARGET --app-name NAME --slug SLUG --option-prefix PREFIX --icon FILE [options]

Options:
  --cli-option NAME          CMake option used to disable the native CLI
  --version VERSION          Package/version-label override
  --build-dir DIR            Default: <source>/build-wasm
  --dist-dir DIR             Default: <source>/dist-wasm
  --jobs N
  --vcpkg-root DIR           vcpkg checkout for manifest dependencies
  --vcpkg-triplet NAME       Default: wasm32-emscripten
  --vcpkg-installed-dir DIR  Persistent vcpkg installed tree
  --cmake-arg ARG            Additional CMake argument (repeatable)
  --clean
  -- ARGS...                 Additional CMake arguments
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
    --version) VERSION="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --dist-dir) DIST_DIR="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-triplet) VCPKG_TRIPLET="$2"; shift 2;;
    --vcpkg-installed-dir) VCPKG_INSTALLED_DIR="$2"; shift 2;;
    --cmake-arg) EXTRA_CMAKE_ARGS+=("$2"); shift 2;;
    --clean) CLEAN=1; shift;;
    --) shift; EXTRA_CMAKE_ARGS+=("$@"); break;;
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

if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  case "$VCPKG_ROOT_VALUE" in /*) ;; *) VCPKG_ROOT_VALUE="$SOURCE_ROOT/$VCPKG_ROOT_VALUE";; esac
  [[ -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]] || {
    echo "vcpkg toolchain file was not found under: $VCPKG_ROOT_VALUE" >&2
    exit 2
  }
  [[ -x "$VCPKG_ROOT_VALUE/vcpkg" ]] || {
    echo "vcpkg is not bootstrapped under: $VCPKG_ROOT_VALUE" >&2
    echo "Run: bash '$VCPKG_ROOT_VALUE/bootstrap-vcpkg.sh' -disableMetrics" >&2
    exit 2
  }
  VCPKG_ROOT_VALUE="$(cd "$VCPKG_ROOT_VALUE" && pwd)"
  [[ -n "$VCPKG_INSTALLED_DIR" ]] || VCPKG_INSTALLED_DIR="$DEPS_ROOT/vcpkg-installed"
  case "$VCPKG_INSTALLED_DIR" in /*) ;; *) VCPKG_INSTALLED_DIR="$SOURCE_ROOT/$VCPKG_INSTALLED_DIR";; esac
  mkdir -p "$VCPKG_INSTALLED_DIR"
  VCPKG_INSTALLED_DIR="$(cd "$VCPKG_INSTALLED_DIR" && pwd)"
fi

bash "$NEOSHARED_ROOT_VALUE/scripts/setup-wasm.sh" --deps-root "$DEPS_ROOT"
bash "$NEOSHARED_ROOT_VALUE/scripts/build-wx-wasm.sh" --deps-root "$DEPS_ROOT" --jobs "$JOBS"
# shellcheck disable=SC1090
source "$DEPS_ROOT/neo-wasm-versions.env"
# shellcheck disable=SC1090
source "$NEO_WASM_EMSDK_ROOT/emsdk_env.sh" >/dev/null
WX_BUILD="$DEPS_ROOT/wxwidgets-wasm-build"
[[ -x "$WX_BUILD/wx-config" ]] || { echo "wx-config was not produced" >&2; exit 1; }
export WX_CONFIG="$WX_BUILD/wx-config"
export EMSCRIPTEN_ROOT="${EMSCRIPTEN_ROOT:-$NEO_WASM_EMSDK_ROOT/upstream/emscripten}"
EMSCRIPTEN_TOOLCHAIN_FILE="$EMSCRIPTEN_ROOT/cmake/Modules/Platform/Emscripten.cmake"
[[ -f "$EMSCRIPTEN_TOOLCHAIN_FILE" ]] || {
  echo "Emscripten CMake toolchain file was not found: $EMSCRIPTEN_TOOLCHAIN_FILE" >&2
  exit 2
}

for command_name in cmake ninja embuilder; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable after activating emsdk: $command_name" >&2
    exit 2
  }
done

echo "wxWidgets-WASM discovery diagnostics:"
echo "  wx-config: $WX_CONFIG"
echo "  selected:  $("$WX_CONFIG" --selected-config)"
echo "  version:   $("$WX_CONFIG" --version)"
echo "  cxxflags:  $("$WX_CONFIG" --cxxflags)"
echo "  libraries: $("$WX_CONFIG" --libs core,base,adv,aui)"
[[ "$CLEAN" == 0 ]] || rm -rf "$BUILD_DIR"

cmake_args=(
  -S "$SOURCE_ROOT"
  -B "$BUILD_DIR"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_FLAGS=-fexceptions
  -DNEOSHARED_ROOT="$NEOSHARED_ROOT_VALUE"
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

configure_command=(emcmake cmake)
if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  configure_command=(cmake)
  cmake_args+=(
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake"
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$EMSCRIPTEN_TOOLCHAIN_FILE"
    -DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET"
    -DVCPKG_MANIFEST_MODE=ON
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"
  )
fi
cmake_args+=("${EXTRA_CMAKE_ARGS[@]}")

embuilder build zlib
"${configure_command[@]}" "${cmake_args[@]}"

CMAKE_CACHE="$BUILD_DIR/CMakeCache.txt"
[[ -f "$CMAKE_CACHE" ]] || {
  echo "CMake did not produce a cache: $CMAKE_CACHE" >&2
  exit 1
}
CMAKE_SYSTEM_FILE="$(find "$BUILD_DIR/CMakeFiles" -name CMakeSystem.cmake -print -quit 2>/dev/null || true)"
if ! grep -Eq '^CMAKE_SYSTEM_NAME:[^=]*=Emscripten$' "$CMAKE_CACHE" &&
   ! { [[ -n "$CMAKE_SYSTEM_FILE" ]] &&
       grep -Eq '^set\(CMAKE_SYSTEM_NAME[[:space:]]+"?Emscripten"?\)' "$CMAKE_SYSTEM_FILE"; }; then
  echo "CMake configured the application for the wrong system; expected Emscripten." >&2
  grep -E '^CMAKE_SYSTEM_NAME:' "$CMAKE_CACHE" >&2 || true
  [[ -z "$CMAKE_SYSTEM_FILE" ]] || grep -E '^set\(CMAKE_SYSTEM_NAME' "$CMAKE_SYSTEM_FILE" >&2 || true
  exit 1
fi
CMAKE_CXX_COMPILER_VALUE="$(sed -nE 's/^CMAKE_CXX_COMPILER:[^=]*=(.*)$/\1/p' "$CMAKE_CACHE" | sed -n '1p')"
if [[ -z "$CMAKE_CXX_COMPILER_VALUE" ]]; then
  CMAKE_CXX_COMPILER_FILE="$(find "$BUILD_DIR/CMakeFiles" -name CMakeCXXCompiler.cmake -print -quit 2>/dev/null || true)"
  if [[ -n "$CMAKE_CXX_COMPILER_FILE" ]]; then
    CMAKE_CXX_COMPILER_VALUE="$(sed -nE 's/^set\(CMAKE_CXX_COMPILER[[:space:]]+"([^"]+)"\).*$/\1/p' "$CMAKE_CXX_COMPILER_FILE" | sed -n '1p')"
  fi
fi
[[ -n "$CMAKE_CXX_COMPILER_VALUE" ]] || {
  echo "CMake did not record a C++ compiler in its cache or compiler metadata." >&2
  exit 1
}
case "$(basename "$CMAKE_CXX_COMPILER_VALUE")" in
  em++|em++.*) ;;
  *)
    echo "CMake selected a non-Emscripten C++ compiler: $CMAKE_CXX_COMPILER_VALUE" >&2
    exit 1
    ;;
esac

cmake --build "$BUILD_DIR" --target "$APP_TARGET" --parallel "$JOBS"

if [[ -z "$VERSION" && -f "$SOURCE_ROOT/src/core/Version.hpp" ]]; then
  VERSION="$(sed -nE 's/^#define[[:space:]]+[A-Z0-9_]+_VERSION_STRING[[:space:]]+"([^"]+)".*/\1/p' "$SOURCE_ROOT/src/core/Version.hpp" | sed -n '1p')"
fi
if [[ -z "$VERSION" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  VERSION="$(sed -nE 's/^CMAKE_PROJECT_VERSION:[^=]*=(.*)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | sed -n '1p')"
fi
if [[ -z "$VERSION" && -f "$SOURCE_ROOT/vcpkg.json" ]]; then
  VERSION="$(sed -nE 's/^[[:space:]]*"version(-string|-semver)?"[[:space:]]*:[[:space:]]*"([^"]+)".*/\2/p' "$SOURCE_ROOT/vcpkg.json" | sed -n '1p')"
fi
[[ -n "$VERSION" ]] || VERSION="snapshot"

bash "$NEOSHARED_ROOT_VALUE/scripts/package-wasm-site.sh" \
  --input "$BUILD_DIR/wasm-output" \
  --dist "$DIST_DIR" \
  --app-name "$APP_NAME" \
  --slug "$SLUG" \
  --version "$VERSION" \
  --icon "$ICON"
echo "GitHub Pages site: $DIST_DIR"
