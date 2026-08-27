#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_BUNDLE=""
OUTPUT_ZIP=""
ARCH=""
APP_NAME=""
VERSION=""
DEPLOYMENT_TARGET=""
EXTRA_SEARCH_DIRS=()

usage() {
  cat <<'USAGE'
usage: package-macos-app.sh --app APP.app --output FILE.zip --arch ARCH [options]

Options:
  --app PATH                 Installed application bundle
  --output PATH              Destination ZIP
  --arch arm64|x86_64        Required native architecture
  --app-name NAME            Application display name
  --version VERSION          Application semantic version
  --deployment-target VER    Requested minimum macOS version
  --search-dir DIR           Additional dependency search directory (repeatable)
  -h, --help                 Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP_BUNDLE="$2"; shift 2;;
    --output) OUTPUT_ZIP="$2"; shift 2;;
    --arch) ARCH="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --deployment-target) DEPLOYMENT_TARGET="$2"; shift 2;;
    --search-dir) EXTRA_SEARCH_DIRS+=("$2"); shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

[[ "$(uname -s)" == "Darwin" ]] || { echo "This script must run on macOS." >&2; exit 2; }
[[ -n "$APP_BUNDLE" && -d "$APP_BUNDLE" ]] || { echo "Missing .app bundle: $APP_BUNDLE" >&2; exit 2; }
[[ -n "$OUTPUT_ZIP" ]] || { echo "--output is required" >&2; exit 2; }
case "$ARCH" in arm64|x86_64) ;; *) echo "--arch must be arm64 or x86_64" >&2; exit 2;; esac

for command_name in cmake codesign ditto file lipo otool plutil shasum; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 2
  }
done

APP_BUNDLE="$(cd "$(dirname "$APP_BUNDLE")" && pwd)/$(basename "$APP_BUNDLE")"
PLIST="$APP_BUNDLE/Contents/Info.plist"
[[ -f "$PLIST" ]] || { echo "Bundle Info.plist is missing: $PLIST" >&2; exit 2; }
PLIST_BUDDY=/usr/libexec/PlistBuddy
[[ -x "$PLIST_BUDDY" ]] || { echo "PlistBuddy is unavailable: $PLIST_BUDDY" >&2; exit 2; }
set_plist_string() {
  local key="$1"
  local value="$2"
  if "$PLIST_BUDDY" -c "Print :$key" "$PLIST" >/dev/null 2>&1; then
    "$PLIST_BUDDY" -c "Set :$key $value" "$PLIST"
  else
    "$PLIST_BUDDY" -c "Add :$key string $value" "$PLIST"
  fi
}
[[ -z "$DEPLOYMENT_TARGET" ]] || set_plist_string LSMinimumSystemVersion "$DEPLOYMENT_TARGET"
EXECUTABLE_NAME="$($PLIST_BUDDY -c 'Print :CFBundleExecutable' "$PLIST")"
EXECUTABLE="$APP_BUNDLE/Contents/MacOS/$EXECUTABLE_NAME"
[[ -f "$EXECUTABLE" ]] || { echo "Bundle executable is missing: $EXECUTABLE" >&2; exit 2; }

SEARCH_DIRS=()
if command -v brew >/dev/null 2>&1; then
  BREW_PREFIX="$(brew --prefix 2>/dev/null || true)"
  WX_PREFIX="$(brew --prefix wxwidgets 2>/dev/null || true)"
  [[ -z "$BREW_PREFIX" ]] || SEARCH_DIRS+=("$BREW_PREFIX/lib")
  [[ -z "$WX_PREFIX" ]] || SEARCH_DIRS+=("$WX_PREFIX/lib")
fi
for search_dir in "${EXTRA_SEARCH_DIRS[@]}"; do
  [[ -d "$search_dir" ]] || continue
  search_dir="$(cd "$search_dir" && pwd)"
  already_present=0
  for existing_dir in "${SEARCH_DIRS[@]}"; do
    if [[ "$existing_dir" == "$search_dir" ]]; then
      already_present=1
      break
    fi
  done
  [[ "$already_present" == 1 ]] || SEARCH_DIRS+=("$search_dir")
done
SEARCH_DIRS_JOINED="$(IFS='|'; printf '%s' "${SEARCH_DIRS[*]:-}")"

cmake \
  "-DAPP_BUNDLE=$APP_BUNDLE" \
  "-DSEARCH_DIRS=$SEARCH_DIRS_JOINED" \
  -P "$ROOT_DIR/cmake/NeoMacOSFixupBundle.cmake"

check_arch() {
  local binary="$1"
  local architectures
  architectures="$(lipo -archs "$binary" 2>/dev/null || true)"
  case " $architectures " in
    *" $ARCH "*) ;;
    *) echo "Required architecture $ARCH is missing: $binary ($architectures)" >&2; return 1;;
  esac
}

check_arch "$EXECUTABLE"
while IFS= read -r -d '' candidate; do
  if file "$candidate" | grep -q 'Mach-O'; then
    check_arch "$candidate"
  fi
done < <(find "$APP_BUNDLE" -type f -print0)

BAD_DEPS="$(mktemp)"
inspect_deps() {
  local binary="$1"
  local dependency
  while IFS= read -r dependency; do
    case "$dependency" in
      @*|/usr/lib/*|/System/Library/*|/Library/Apple/System/Library/*) ;;
      *) printf '%s -> %s\n' "$binary" "$dependency" >> "$BAD_DEPS";;
    esac
  done < <(otool -L "$binary" | sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
}
inspect_deps "$EXECUTABLE"
while IFS= read -r -d '' candidate; do
  if file "$candidate" | grep -q 'Mach-O'; then
    inspect_deps "$candidate"
  fi
done < <(find "$APP_BUNDLE" -type f -print0)
if [[ -s "$BAD_DEPS" ]]; then
  echo "The bundle still contains non-system absolute library references:" >&2
  cat "$BAD_DEPS" >&2
  rm -f "$BAD_DEPS"
  exit 1
fi
rm -f "$BAD_DEPS"

xattr -cr "$APP_BUNDLE" 2>/dev/null || true
rm -rf "$APP_BUNDLE/Contents/_CodeSignature"
while IFS= read -r -d '' candidate; do
  if file "$candidate" | grep -q 'Mach-O'; then
    codesign --force --sign - --timestamp=none "$candidate"
  fi
done < <(find "$APP_BUNDLE/Contents" -type f -print0)
codesign --force --sign - --timestamp=none "$APP_BUNDLE"
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"
plutil -lint "$PLIST"

mkdir -p "$(dirname "$OUTPUT_ZIP")"
rm -f "$OUTPUT_ZIP" "$OUTPUT_ZIP.sha256"
ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE" "$OUTPUT_ZIP"
DIGEST="$(shasum -a 256 "$OUTPUT_ZIP" | awk '{print $1}')"
printf '%s  %s\n' "$DIGEST" "$(basename "$OUTPUT_ZIP")" > "$OUTPUT_ZIP.sha256"
printf 'Created %s\n' "$OUTPUT_ZIP"
printf 'SHA-256 %s\n' "$DIGEST"
