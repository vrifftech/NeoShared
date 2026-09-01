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

for command_name in awk cmake codesign ditto find grep lipo mktemp otool plutil readlink sed shasum sort xattr; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 2
  }
done

APP_BUNDLE="$(cd -P "$APP_BUNDLE" && pwd)"
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
for search_dir in "${EXTRA_SEARCH_DIRS[@]+"${EXTRA_SEARCH_DIRS[@]}"}"; do
  [[ -d "$search_dir" ]] || continue
  search_dir="$(cd "$search_dir" && pwd)"
  already_present=0
  for existing_dir in "${SEARCH_DIRS[@]+"${SEARCH_DIRS[@]}"}"; do
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

is_macho_file() {
  otool -hv "$1" >/dev/null 2>&1
}

resolve_bundle_file() {
  local path="$1"
  local directory
  local link_target
  local hops=0

  while [[ -L "$path" ]]; do
    hops=$((hops + 1))
    if [[ "$hops" -gt 64 ]]; then
      echo "Too many symbolic-link hops inside the application bundle: $1" >&2
      return 1
    fi
    directory="$(cd -P "$(dirname "$path")" && pwd)"
    link_target="$(readlink "$path")"
    case "$link_target" in
      /*) path="$link_target";;
      *) path="$directory/$link_target";;
    esac
  done

  directory="$(cd -P "$(dirname "$path")" && pwd)"
  printf '%s/%s\n' "$directory" "$(basename "$path")"
}

EXECUTABLE_RESOLVED="$(resolve_bundle_file "$EXECUTABLE")"
[[ -f "$EXECUTABLE_RESOLVED" ]] || {
  echo "Bundle executable resolves to a missing file: $EXECUTABLE -> $EXECUTABLE_RESOLVED" >&2
  exit 1
}

MACHO_PATHS="$(mktemp)"
BAD_DEPS="$(mktemp)"
cleanup_temporary_files() {
  rm -f "$MACHO_PATHS" "$BAD_DEPS"
}
trap cleanup_temporary_files EXIT

# Build one canonical Mach-O inventory and reuse it for architecture,
# dependency, signing, and verification checks. Include symbolic links so
# Homebrew's versioned Intel dylib layouts cannot bypass the signing pass.
while IFS= read -r -d '' candidate; do
  resolved_candidate="$(resolve_bundle_file "$candidate")"
  case "$resolved_candidate" in
    "$APP_BUNDLE"/*) ;;
    *)
      echo "Bundle symbolic link resolves outside the application: $candidate -> $resolved_candidate" >&2
      exit 1
      ;;
  esac
  if [[ -d "$resolved_candidate" ]]; then
    continue
  fi
  [[ -f "$resolved_candidate" ]] || {
    echo "Bundle entry resolves to a missing file: $candidate -> $resolved_candidate" >&2
    exit 1
  }
  if is_macho_file "$resolved_candidate"; then
    printf '%s\n' "$resolved_candidate" >> "$MACHO_PATHS"
  fi
done < <(find "$APP_BUNDLE/Contents" \
  \( -type f -o -type l \) -print0)
LC_ALL=C sort -u -o "$MACHO_PATHS" "$MACHO_PATHS"

if ! grep -Fqx -- "$EXECUTABLE_RESOLVED" "$MACHO_PATHS"; then
  echo "The main executable was not identified as Mach-O: $EXECUTABLE_RESOLVED" >&2
  exit 1
fi

while IFS= read -r candidate; do
  [[ -n "$candidate" ]] || continue
  check_arch "$candidate"
done < "$MACHO_PATHS"

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
while IFS= read -r candidate; do
  [[ -n "$candidate" ]] || continue
  inspect_deps "$candidate"
done < "$MACHO_PATHS"
if [[ -s "$BAD_DEPS" ]]; then
  echo "The bundle still contains non-system absolute library references:" >&2
  cat "$BAD_DEPS" >&2
  exit 1
fi

sign_file() {
  local candidate="$1"
  printf 'Signing Mach-O: %s\n' "$candidate"
  codesign --force --sign - --timestamp=none "$candidate" </dev/null
  codesign --verify --strict --verbose=2 "$candidate" </dev/null
}

sign_bundle() {
  local candidate="$1"
  printf 'Signing nested bundle: %s\n' "$candidate"
  codesign --force --sign - --timestamp=none "$candidate" </dev/null
  codesign --verify --deep --strict --verbose=2 "$candidate" </dev/null
}

xattr -cr "$APP_BUNDLE" 2>/dev/null || true
find "$APP_BUNDLE" -type d -name _CodeSignature -prune -exec rm -rf {} +

# Sign all embedded Mach-O code first. Redirect stdin so codesign cannot
# consume the path inventory used by the surrounding loop on Bash 3.2.
# The main executable is signed after nested bundles, immediately before the
# outer application bundle.
while IFS= read -r candidate; do
  [[ -n "$candidate" ]] || continue
  [[ "$candidate" == "$EXECUTABLE_RESOLVED" ]] && continue
  sign_file "$candidate"
done < "$MACHO_PATHS"

# Sign nested code containers from the deepest level outward, as required by
# Apple's manual-signing model. Plain dylibs in Contents/Frameworks were
# already signed by the Mach-O pass above.
while IFS= read -r -d '' nested_bundle; do
  if grep -Fq -- "$nested_bundle/" "$MACHO_PATHS"; then
    sign_bundle "$nested_bundle"
  fi
done < <(find "$APP_BUNDLE/Contents" -depth -type d \
  \( -name '*.app' -o -name '*.framework' -o -name '*.xpc' \
     -o -name '*.appex' -o -name '*.plugin' -o -name '*.bundle' \) -print0)

sign_file "$EXECUTABLE_RESOLVED"
printf 'Signing application bundle: %s\n' "$APP_BUNDLE"
codesign --force --sign - --timestamp=none "$APP_BUNDLE" </dev/null

# Finish with a recursive ad-hoc signing pass. This covers versioned Homebrew
# dylibs and nested code containers even when aliases resolve to a path that
# was not represented independently in the first canonical inventory.
printf 'Finalizing nested code signatures: %s\n' "$APP_BUNDLE"
codesign --force --deep --sign - --timestamp=none "$APP_BUNDLE" </dev/null

# Verify each concrete Mach-O independently before the recursive app check so
# an unsigned Intel/Homebrew dylib is reported by its exact path.
while IFS= read -r candidate; do
  [[ -n "$candidate" ]] || continue
  codesign --verify --strict --verbose=2 "$candidate" </dev/null
done < "$MACHO_PATHS"
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE" </dev/null
plutil -lint "$PLIST"

mkdir -p "$(dirname "$OUTPUT_ZIP")"
rm -f "$OUTPUT_ZIP" "$OUTPUT_ZIP.sha256"
ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE" "$OUTPUT_ZIP"
DIGEST="$(shasum -a 256 "$OUTPUT_ZIP" | awk '{print $1}')"
printf '%s  %s\n' "$DIGEST" "$(basename "$OUTPUT_ZIP")" > "$OUTPUT_ZIP.sha256"
printf 'Created %s\n' "$OUTPUT_ZIP"
printf 'SHA-256 %s\n' "$DIGEST"
