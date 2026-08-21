#!/usr/bin/env bash
set -euo pipefail

SOURCE_ROOT=""
APP_NAME=""
NEOSHARED_ROOT_VALUE=""
ARCH="$(uname -m)"
DEPLOYMENT_TARGET="15.0"
BUILD_TYPE="Release"
BUILD_DIR=""
STAGE_DIR=""
DIST_DIR=""
JOBS=""
CLEAN=0
RUN_TESTS=1

usage() {
  cat <<'USAGE'
usage: build-macos-app.sh --source-root DIR --app-name NAME --neoshared-root DIR [options]

Options:
  --arch arm64|x86_64       Native architecture [default: uname -m]
  --deployment-target VER   Requested minimum macOS version [default: 15.0]
  --build-type TYPE         CMake build type [default: Release]
  --build-dir DIR           Build directory
  --stage-dir DIR           Install staging directory
  --dist-dir DIR            ZIP output directory
  --jobs N                  Parallel build jobs
  --clean                   Remove build and stage directories first
  --skip-tests              Do not run existing CTest tests
  -h, --help                Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-root) SOURCE_ROOT="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --arch) ARCH="$2"; shift 2;;
    --deployment-target) DEPLOYMENT_TARGET="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --stage-dir) STAGE_DIR="$2"; shift 2;;
    --dist-dir) DIST_DIR="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    --skip-tests) RUN_TESTS=0; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

[[ "$(uname -s)" == "Darwin" ]] || { echo "This build script must run on macOS." >&2; exit 2; }
[[ -n "$SOURCE_ROOT" && -d "$SOURCE_ROOT" ]] || { echo "--source-root is required" >&2; exit 2; }
[[ -n "$APP_NAME" ]] || { echo "--app-name is required" >&2; exit 2; }
[[ -n "$NEOSHARED_ROOT_VALUE" && -f "$NEOSHARED_ROOT_VALUE/CMakeLists.txt" ]] || {
  echo "--neoshared-root must point to the sibling neoshared repository" >&2
  exit 2
}
case "$ARCH" in arm64|x86_64) ;; *) echo "Unsupported architecture: $ARCH" >&2; exit 2;; esac
[[ "$(uname -m)" == "$ARCH" ]] || {
  echo "Cross-architecture packaging is disabled." >&2
  echo "Requested $ARCH, but this host is $(uname -m). Use a matching native Mac or runner." >&2
  exit 2
}

SOURCE_ROOT="$(cd "$SOURCE_ROOT" && pwd)"
NEOSHARED_ROOT_VALUE="$(cd "$NEOSHARED_ROOT_VALUE" && pwd)"
[[ -n "$BUILD_DIR" ]] || BUILD_DIR="$SOURCE_ROOT/build-macos-$ARCH"
[[ -n "$STAGE_DIR" ]] || STAGE_DIR="$SOURCE_ROOT/stage-macos-$ARCH"
[[ -n "$DIST_DIR" ]] || DIST_DIR="$SOURCE_ROOT/dist"
resolve_output_path() {
  local label="$1"
  local value="$2"
  case "$value" in /*) ;; *) value="$SOURCE_ROOT/$value";; esac
  case "$value" in
    *"/../"*|*/..|"$SOURCE_ROOT")
      echo "$label must be a child directory of the application repository: $value" >&2
      exit 2
      ;;
  esac
  case "$value" in
    "$SOURCE_ROOT"/*) ;;
    *) echo "$label must be inside the application repository: $value" >&2; exit 2;;
  esac
  printf '%s' "$value"
}
BUILD_DIR="$(resolve_output_path --build-dir "$BUILD_DIR")"
STAGE_DIR="$(resolve_output_path --stage-dir "$STAGE_DIR")"
DIST_DIR="$(resolve_output_path --dist-dir "$DIST_DIR")"
[[ -n "$JOBS" ]] || JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf 2)"

for command_name in brew cmake ninja; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    echo "Install prerequisites with: brew install cmake ninja wxwidgets" >&2
    exit 2
  }
done

WX_PREFIX="$(brew --prefix wxwidgets)"
export PATH="$WX_PREFIX/bin:$(brew --prefix)/bin:$PATH"
export CMAKE_PREFIX_PATH="$WX_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$WX_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
command -v wx-config >/dev/null 2>&1 || {
  echo "wx-config is unavailable under the Homebrew wxWidgets prefix: $WX_PREFIX" >&2
  exit 2
}
printf 'wxWidgets %s from %s\n' "$(wx-config --version)" "$(command -v wx-config)"

clean_args=()
[[ "$CLEAN" == 0 ]] || clean_args+=(--clean)
if [[ "$CLEAN" == 1 ]]; then
  rm -rf -- "$STAGE_DIR"
fi

bash "$SOURCE_ROOT/scripts/build.sh" \
  --build-dir "$BUILD_DIR" \
  --build-type "$BUILD_TYPE" \
  --wx ON \
  --require-wx ON \
  --cli ON \
  --minimal-release ON \
  --generator Ninja \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  --no-vcpkg \
  --jobs "$JOBS" \
  "${clean_args[@]}" \
  -- \
  "-DCMAKE_OSX_ARCHITECTURES=$ARCH" \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET"

if [[ "$RUN_TESTS" == 1 ]]; then
  ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure
fi

rm -rf -- "$STAGE_DIR"
cmake --install "$BUILD_DIR" --config "$BUILD_TYPE" --prefix "$STAGE_DIR"
APP_BUNDLE="$STAGE_DIR/$APP_NAME.app"
[[ -d "$APP_BUNDLE" ]] || { echo "Installed application bundle is missing: $APP_BUNDLE" >&2; exit 1; }

VERSION="$(sed -nE 's/^#define[[:space:]]+[A-Z0-9_]+_VERSION_STRING[[:space:]]+"([^"]+)".*/\1/p' "$SOURCE_ROOT/src/core/Version.hpp" | sed -n '1p')"
[[ -n "$VERSION" ]] || { echo "Unable to read src/core/Version.hpp" >&2; exit 1; }
OUTPUT_ZIP="$DIST_DIR/$APP_NAME-$VERSION-macos-$ARCH.zip"

bash "$NEOSHARED_ROOT_VALUE/scripts/package-macos-app.sh" \
  --app "$APP_BUNDLE" \
  --output "$OUTPUT_ZIP" \
  --arch "$ARCH" \
  --app-name "$APP_NAME" \
  --version "$VERSION" \
  --deployment-target "$DEPLOYMENT_TARGET"
