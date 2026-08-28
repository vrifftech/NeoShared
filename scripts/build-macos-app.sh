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
VERSION=""
VCPKG_ROOT_VALUE=""
VCPKG_TRIPLET=""
EXTRA_CMAKE_ARGS=()

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
  --version VERSION         Package version override
  --vcpkg-root DIR          vcpkg checkout for manifest dependencies
  --vcpkg-triplet NAME      vcpkg target triplet [default: native *-osx]
  --cmake-arg ARG           Additional CMake argument (repeatable)
  --clean                   Remove build and stage directories first
  -- ARGS...                Additional CMake arguments
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
    --version) VERSION="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-triplet) VCPKG_TRIPLET="$2"; shift 2;;
    --cmake-arg) EXTRA_CMAKE_ARGS+=("$2"); shift 2;;
    --clean) CLEAN=1; shift;;
    --) shift; EXTRA_CMAKE_ARGS+=("$@"); break;;
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
if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  case "$VCPKG_ROOT_VALUE" in /*) ;; *) VCPKG_ROOT_VALUE="$SOURCE_ROOT/$VCPKG_ROOT_VALUE";; esac
  [[ -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]] || {
    echo "vcpkg toolchain file is missing under: $VCPKG_ROOT_VALUE" >&2
    exit 2
  }
  VCPKG_ROOT_VALUE="$(cd "$VCPKG_ROOT_VALUE" && pwd)"
  [[ -x "$VCPKG_ROOT_VALUE/vcpkg" ]] || {
    echo "vcpkg is not bootstrapped under: $VCPKG_ROOT_VALUE" >&2
    echo "Run: bash '$VCPKG_ROOT_VALUE/bootstrap-vcpkg.sh' -disableMetrics" >&2
    exit 2
  }
  if [[ -z "$VCPKG_TRIPLET" ]]; then
    case "$ARCH" in
      arm64) VCPKG_TRIPLET="arm64-osx";;
      x86_64) VCPKG_TRIPLET="x64-osx";;
    esac
  fi
fi
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

build_args=(
  --build-dir "$BUILD_DIR"
  --build-type "$BUILD_TYPE"
  --wx ON
  --require-wx ON
  --cli ON
  --minimal-release ON
  --generator Ninja
  --neoshared-root "$NEOSHARED_ROOT_VALUE"
  --jobs "$JOBS"
)
if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  build_args+=(
    --vcpkg-root "$VCPKG_ROOT_VALUE"
    --vcpkg-triplet "$VCPKG_TRIPLET"
  )
else
  build_args+=(--no-vcpkg)
fi
build_args+=("${clean_args[@]+"${clean_args[@]}"}")

bash "$SOURCE_ROOT/scripts/build.sh" \
  "${build_args[@]}" \
  -- \
  "-DCMAKE_OSX_ARCHITECTURES=$ARCH" \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET" \
  "-DwxWidgets_CONFIG_EXECUTABLE=$WX_PREFIX/bin/wx-config" \
  "${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}"

rm -rf -- "$STAGE_DIR"
cmake --install "$BUILD_DIR" --config "$BUILD_TYPE" --prefix "$STAGE_DIR"
APP_BUNDLE="$STAGE_DIR/$APP_NAME.app"
[[ -d "$APP_BUNDLE" ]] || { echo "Installed application bundle is missing: $APP_BUNDLE" >&2; exit 1; }

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
VERSION_FILE_COMPONENT="$(printf '%s' "$VERSION" | LC_ALL=C tr -c 'A-Za-z0-9._-' '-')"
[[ -n "$VERSION_FILE_COMPONENT" ]] || VERSION_FILE_COMPONENT="snapshot"
OUTPUT_ZIP="$DIST_DIR/$APP_NAME-$VERSION_FILE_COMPONENT-macos-$ARCH.zip"

package_args=(
  --app "$APP_BUNDLE"
  --output "$OUTPUT_ZIP"
  --arch "$ARCH"
  --app-name "$APP_NAME"
  --version "$VERSION"
  --deployment-target "$DEPLOYMENT_TARGET"
)
if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  for search_dir in \
    "$BUILD_DIR/vcpkg_installed/$VCPKG_TRIPLET/lib" \
    "$BUILD_DIR/vcpkg_installed/$VCPKG_TRIPLET/debug/lib" \
    "$VCPKG_ROOT_VALUE/installed/$VCPKG_TRIPLET/lib" \
    "$VCPKG_ROOT_VALUE/installed/$VCPKG_TRIPLET/debug/lib"; do
    [[ -d "$search_dir" ]] || continue
    package_args+=(--search-dir "$search_dir")
  done
fi

bash "$NEOSHARED_ROOT_VALUE/scripts/package-macos-app.sh" "${package_args[@]}"
