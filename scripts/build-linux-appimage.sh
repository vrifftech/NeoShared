#!/usr/bin/env bash
set -euo pipefail

SOURCE_ROOT=""
APP_NAME=""
NEOSHARED_ROOT_VALUE=""
BUILD_TYPE="Release"
BUILD_DIR=""
APPDIR=""
DIST_DIR=""
JOBS="${JOBS:-}"
CLEAN=0
VERSION=""
BUILD_CLI="OFF"
VCPKG_ROOT_VALUE=""
VCPKG_TRIPLET=""
LINUXDEPLOY_PATH="${NEO_LINUXDEPLOY:-}"
LINUXDEPLOY_URL="${NEO_LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage}"
LINUXDEPLOY_SHA256="${NEO_LINUXDEPLOY_SHA256:-}"
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
usage: build-linux-appimage.sh --source-root DIR --app-name NAME --neoshared-root DIR [options]

Builds one x86-64 Linux AppImage from a NeoTools wxWidgets application.
The application repository must provide scripts/build.sh and install its GUI,
.desktop file, and icon through CMake.

Options:
  --build-type TYPE          CMake build type [default: Release]
  --build-dir DIR            Build directory [default: build-linux-x86_64]
  --appdir DIR               AppDir staging directory [default: AppDir-x86_64]
  --dist-dir DIR             Output directory [default: dist]
  --jobs N                   Parallel build jobs
  --version VERSION          Package version override
  --cli ON|OFF               Also build/install the CLI [default: OFF]
  --vcpkg-root DIR           vcpkg checkout for manifest dependencies
  --vcpkg-triplet NAME       vcpkg target triplet [default: x64-linux]
  --linuxdeploy FILE         Existing linuxdeploy x86_64 AppImage
  --linuxdeploy-url URL      Download URL when --linuxdeploy is not supplied
  --linuxdeploy-sha256 HEX   Optional SHA-256 for the downloaded linuxdeploy
  --cmake-arg ARG            Additional CMake argument (repeatable)
  --clean                    Remove build/AppDir before building
  -- ARGS...                 Additional CMake arguments
  -h, --help                 Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-root) SOURCE_ROOT="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --appdir) APPDIR="$2"; shift 2;;
    --dist-dir) DIST_DIR="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --cli) BUILD_CLI="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-triplet) VCPKG_TRIPLET="$2"; shift 2;;
    --linuxdeploy) LINUXDEPLOY_PATH="$2"; shift 2;;
    --linuxdeploy-url) LINUXDEPLOY_URL="$2"; shift 2;;
    --linuxdeploy-sha256) LINUXDEPLOY_SHA256="$2"; shift 2;;
    --cmake-arg) EXTRA_CMAKE_ARGS+=("$2"); shift 2;;
    --cmake-arg=*) EXTRA_CMAKE_ARGS+=("${1#*=}"); shift;;
    --clean) CLEAN=1; shift;;
    --) shift; EXTRA_CMAKE_ARGS+=("$@"); break;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

[[ "$(uname -s)" == "Linux" ]] || { echo "This script must run on Linux." >&2; exit 2; }
case "$(uname -m)" in
  x86_64|amd64) ;;
  *) echo "Only x86-64 AppImages are supported; host architecture is $(uname -m)." >&2; exit 2;;
esac
[[ -n "$SOURCE_ROOT" && -d "$SOURCE_ROOT" ]] || { echo "--source-root is required" >&2; exit 2; }
[[ -n "$APP_NAME" ]] || { echo "--app-name is required" >&2; exit 2; }
[[ -n "$NEOSHARED_ROOT_VALUE" && -f "$NEOSHARED_ROOT_VALUE/CMakeLists.txt" ]] || {
  echo "--neoshared-root must point to the sibling neoshared repository" >&2
  exit 2
}

SOURCE_ROOT="$(cd "$SOURCE_ROOT" && pwd)"
NEOSHARED_ROOT_VALUE="$(cd "$NEOSHARED_ROOT_VALUE" && pwd)"
[[ -f "$SOURCE_ROOT/scripts/build.sh" ]] || {
  echo "Application build wrapper is missing: $SOURCE_ROOT/scripts/build.sh" >&2
  exit 2
}

resolve_output_path() {
  local label="$1"
  local value="$2"
  case "$value" in /*) ;; *) value="$SOURCE_ROOT/$value";; esac
  case "$value" in
    "$SOURCE_ROOT") echo "$label must not be the repository root" >&2; exit 2;;
    "$SOURCE_ROOT"/*) ;;
    *) echo "$label must be inside the application repository: $value" >&2; exit 2;;
  esac
  printf '%s' "$value"
}

[[ -n "$BUILD_DIR" ]] || BUILD_DIR="build-linux-x86_64"
[[ -n "$APPDIR" ]] || APPDIR="AppDir-x86_64"
[[ -n "$DIST_DIR" ]] || DIST_DIR="dist"
BUILD_DIR="$(resolve_output_path --build-dir "$BUILD_DIR")"
APPDIR="$(resolve_output_path --appdir "$APPDIR")"
DIST_DIR="$(resolve_output_path --dist-dir "$DIST_DIR")"
[[ -n "$JOBS" ]] || JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 2)"

for command_name in cmake ninja curl file sha256sum ldd awk sed find; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 2
  }
done
command -v wx-config >/dev/null 2>&1 || {
  echo "wx-config was not found. Install the Linux wxWidgets development package first." >&2
  echo "Debian/Ubuntu: sudo apt install libwxgtk3.2-dev" >&2
  exit 2
}

if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  case "$VCPKG_ROOT_VALUE" in /*) ;; *) VCPKG_ROOT_VALUE="$SOURCE_ROOT/$VCPKG_ROOT_VALUE";; esac
  [[ -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]] || {
    echo "vcpkg toolchain file is missing under: $VCPKG_ROOT_VALUE" >&2
    exit 2
  }
  VCPKG_ROOT_VALUE="$(cd "$VCPKG_ROOT_VALUE" && pwd)"
  [[ -n "$VCPKG_TRIPLET" ]] || VCPKG_TRIPLET="x64-linux"
fi

if [[ "$CLEAN" == 1 ]]; then
  rm -rf -- "$BUILD_DIR" "$APPDIR"
fi
mkdir -p "$DIST_DIR"

build_args=(
  --build-dir "$BUILD_DIR"
  --build-type "$BUILD_TYPE"
  --wx ON
  --require-wx ON
  --cli "$BUILD_CLI"
  --minimal-release ON
  --generator Ninja
  --neoshared-root "$NEOSHARED_ROOT_VALUE"
  --jobs "$JOBS"
)
[[ "$CLEAN" == 0 ]] || build_args+=(--clean)
if [[ -n "$VCPKG_ROOT_VALUE" ]]; then
  build_args+=(--vcpkg-root "$VCPKG_ROOT_VALUE" --vcpkg-triplet "$VCPKG_TRIPLET")
else
  build_args+=(--no-vcpkg)
fi

bash "$SOURCE_ROOT/scripts/build.sh" \
  "${build_args[@]}" \
  -- \
  "-DwxWidgets_CONFIG_EXECUTABLE=$(command -v wx-config)" \
  "${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}"

rm -rf -- "$APPDIR"
cmake --install "$BUILD_DIR" --config "$BUILD_TYPE" --prefix "$APPDIR/usr"

EXECUTABLE="$APPDIR/usr/bin/$APP_NAME"
[[ -x "$EXECUTABLE" ]] || {
  echo "Installed GUI executable is missing: $EXECUTABLE" >&2
  exit 1
}

mapfile -t DESKTOP_FILES < <(find "$APPDIR/usr/share/applications" -maxdepth 1 -type f -name '*.desktop' -print 2>/dev/null | sort)
if [[ ${#DESKTOP_FILES[@]} -ne 1 ]]; then
  echo "Expected exactly one installed .desktop file, found ${#DESKTOP_FILES[@]}." >&2
  printf '  %s\n' "${DESKTOP_FILES[@]}" >&2
  exit 1
fi
DESKTOP_FILE="${DESKTOP_FILES[0]}"
ICON_NAME="$(sed -nE 's/^Icon=([^[:space:]]+).*$/\1/p' "$DESKTOP_FILE" | sed -n '1p')"
[[ -n "$ICON_NAME" ]] || { echo "Desktop file has no Icon= entry: $DESKTOP_FILE" >&2; exit 1; }

mapfile -t ICON_FILES < <(
  find "$APPDIR/usr/share/icons" -type f \
    \( -name "$ICON_NAME.svg" -o -name "$ICON_NAME.png" -o -name "$ICON_NAME.xpm" \) \
    -print 2>/dev/null | sort
)
if [[ ${#ICON_FILES[@]} -eq 0 ]]; then
  echo "Installed icon '$ICON_NAME' was not found under $APPDIR/usr/share/icons." >&2
  exit 1
fi

if [[ -z "$VERSION" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  VERSION="$(sed -nE 's/^CMAKE_PROJECT_VERSION:[^=]*=(.*)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | sed -n '1p')"
fi
[[ -n "$VERSION" ]] || VERSION="snapshot"
VERSION_FILE_COMPONENT="$(printf '%s' "$VERSION" | LC_ALL=C tr -c 'A-Za-z0-9._-' '-')"
[[ -n "$VERSION_FILE_COMPONENT" ]] || VERSION_FILE_COMPONENT="snapshot"
OUTPUT="$DIST_DIR/$APP_NAME-$VERSION_FILE_COMPONENT-linux-x86_64.AppImage"

if [[ -z "$LINUXDEPLOY_PATH" ]]; then
  TOOL_DIR="$SOURCE_ROOT/.cache/appimage-tools"
  mkdir -p "$TOOL_DIR"
  LINUXDEPLOY_PATH="$TOOL_DIR/linuxdeploy-x86_64.AppImage"
  if [[ ! -f "$LINUXDEPLOY_PATH" ]]; then
    echo "Downloading linuxdeploy..."
    curl --fail --location --retry 3 --output "$LINUXDEPLOY_PATH.tmp" "$LINUXDEPLOY_URL"
    mv "$LINUXDEPLOY_PATH.tmp" "$LINUXDEPLOY_PATH"
  fi
fi
case "$LINUXDEPLOY_PATH" in /*) ;; *) LINUXDEPLOY_PATH="$SOURCE_ROOT/$LINUXDEPLOY_PATH";; esac
[[ -f "$LINUXDEPLOY_PATH" ]] || { echo "linuxdeploy was not found: $LINUXDEPLOY_PATH" >&2; exit 2; }
chmod +x "$LINUXDEPLOY_PATH"
if [[ -n "$LINUXDEPLOY_SHA256" ]]; then
  printf '%s  %s\n' "$LINUXDEPLOY_SHA256" "$LINUXDEPLOY_PATH" | sha256sum --check --status || {
    echo "linuxdeploy SHA-256 did not match." >&2
    exit 2
  }
fi

if ldd "$EXECUTABLE" | grep -q 'not found'; then
  echo "The staged executable has unresolved shared-library dependencies:" >&2
  ldd "$EXECUTABLE" >&2
  exit 1
fi

mapfile -t ELF_EXECUTABLES < <(
  find "$APPDIR/usr" -type f -perm -u+x -print0 2>/dev/null \
    | while IFS= read -r -d '' candidate; do
        if file "$candidate" | grep -q 'ELF .* executable'; then
          printf '%s\n' "$candidate"
        fi
      done \
    | sort -u
)
if [[ ${#ELF_EXECUTABLES[@]} -eq 0 ]]; then
  echo "No ELF executables were found in the installed AppDir." >&2
  exit 1
fi
linuxdeploy_args=(--appdir "$APPDIR")
for candidate in "${ELF_EXECUTABLES[@]}"; do
  linuxdeploy_args+=(--executable "$candidate")
done
linuxdeploy_args+=(--desktop-file "$DESKTOP_FILE")
for icon_file in "${ICON_FILES[@]}"; do
  linuxdeploy_args+=(--icon-file "$icon_file")
done
linuxdeploy_args+=(--output appimage)

rm -f -- "$OUTPUT" "$OUTPUT.sha256"
export LDAI_OUTPUT="$OUTPUT"
export LINUXDEPLOY_OUTPUT_APP_NAME="$APP_NAME"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
export APPIMAGE_EXTRACT_AND_RUN=1
"$LINUXDEPLOY_PATH" "${linuxdeploy_args[@]}"

[[ -x "$OUTPUT" ]] || { echo "AppImage was not created: $OUTPUT" >&2; exit 1; }
file "$OUTPUT" | grep -Eq 'ELF 64-bit|AppImage' || {
  echo "Output does not look like an x86-64 AppImage: $OUTPUT" >&2
  file "$OUTPUT" >&2
  exit 1
}
(
  cd "$DIST_DIR"
  sha256sum "$(basename "$OUTPUT")" > "$(basename "$OUTPUT").sha256"
)
printf 'Created %s\n' "$OUTPUT"
