#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR=""
DIST_DIR=""
APP_NAME=""
SLUG=""
VERSION=""
ICON=""
SOURCE_ROOT=""
NEOSHARED_ROOT_VALUE=""
WX_SOURCE=""

usage() {
  cat <<'USAGE'
usage: package-wasm-site.sh --input DIR --dist DIR --app-name NAME --slug SLUG --version VERSION --icon FILE --source-root DIR --neoshared-root DIR --wx-source DIR
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --input) INPUT_DIR="$2"; shift 2;;
    --dist) DIST_DIR="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --slug) SLUG="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --icon) ICON="$2"; shift 2;;
    --source-root) SOURCE_ROOT="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --wx-source) WX_SOURCE="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done
for value in INPUT_DIR DIST_DIR APP_NAME SLUG VERSION ICON SOURCE_ROOT NEOSHARED_ROOT_VALUE WX_SOURCE; do
  [[ -n "${!value}" ]] || { echo "Missing required option: $value" >&2; exit 2; }
done
HTML="$INPUT_DIR/$SLUG.html"
[[ -f "$HTML" ]] || { echo "Missing Emscripten HTML output: $HTML" >&2; exit 1; }
[[ -f "$INPUT_DIR/$SLUG.js" ]] || { echo "Missing Emscripten JavaScript output" >&2; exit 1; }
[[ -f "$INPUT_DIR/$SLUG.wasm" ]] || { echo "Missing WebAssembly output" >&2; exit 1; }
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
cp "$HTML" "$DIST_DIR/index.html"
find "$INPUT_DIR" -maxdepth 1 -type f -name "$SLUG.*" ! -name "$SLUG.html" -exec cp {} "$DIST_DIR/" \;
cp "$ICON" "$DIST_DIR/$SLUG.svg"
cp "$ICON" "$DIST_DIR/favicon.svg"
: > "$DIST_DIR/.nojekyll"

cat > "$DIST_DIR/site.webmanifest" <<EOF
{
  "name": "$APP_NAME",
  "short_name": "$APP_NAME",
  "start_url": "./",
  "display": "standalone",
  "background_color": "#20242b",
  "theme_color": "#20242b",
  "icons": [{"src":"$SLUG.svg","sizes":"any","type":"image/svg+xml"}]
}
EOF

app_revision="unknown"
shared_revision="unknown"
wx_revision="unknown"
[[ -d "$SOURCE_ROOT/.git" ]] && app_revision="$(git -C "$SOURCE_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
[[ -d "$NEOSHARED_ROOT_VALUE/.git" ]] && shared_revision="$(git -C "$NEOSHARED_ROOT_VALUE" rev-parse HEAD 2>/dev/null || echo unknown)"
[[ -d "$WX_SOURCE/.git" ]] && wx_revision="$(git -C "$WX_SOURCE" rev-parse HEAD 2>/dev/null || echo unknown)"
cat > "$DIST_DIR/BUILD_INFO.txt" <<EOF
Application: $APP_NAME
Version: $VERSION
Target: wasm32-unknown-emscripten
Emscripten: 4.0.2
wxWidgets-WASM commit: $wx_revision
Expected wxWidgets-WASM commit: bca69b9fddc88adec57b05e6809467ef9f5158c8
Application revision: $app_revision
neoshared revision: $shared_revision
Threading: single-threaded
Exception model: Emscripten JavaScript exceptions
Persistent settings: wxConfig/localStorage; IDBFS mounted for browser-owned files
Built UTC: $(date -u '+%Y-%m-%dT%H:%M:%SZ')
EOF

cat > "$DIST_DIR/THIRD_PARTY_NOTICES.txt" <<EOF
$APP_NAME WebAssembly browser build

wxWidgets-WASM
Repository: https://github.com/PCBJam/wxWidgets
Pinned commit: bca69b9fddc88adec57b05e6809467ef9f5158c8
The WebAssembly-specific wxWidgets port sources identify LGPL-2.0 terms.
The remainder of wxWidgets retains its upstream licensing terms.
Corresponding source and build instructions are available from the pinned
repository commit and the public $APP_NAME/neoshared repositories used to
produce this build. Review those terms before public redistribution.

Emscripten SDK
Version: 4.0.2
Repository: https://github.com/emscripten-core/emsdk
EOF

cat > "$DIST_DIR/README.txt" <<EOF
$APP_NAME $VERSION browser edition

Open index.html through an HTTP server; WebAssembly is not expected to run
correctly from a file:// URL. GitHub Pages supplies the required static host.
Files selected in the application are handled by the browser and the
Emscripten virtual filesystem. Non-path preferences are stored locally in browser storage.
EOF

cat > "$DIST_DIR/PORT_STATUS.txt" <<EOF
$APP_NAME WebAssembly preview

Available:
- Existing C++/wxWidgets editor UI through wxWidgets-WASM
- Explicit browser file import and individual-file download
- Clipboard operations
- Persistent non-path preferences through wxConfig/localStorage
- Patcher Fragment preview/copy/download where the desktop tool supports it

Desktop-only in this preview:
- Installed-game directory discovery and scanning
- Unrestricted directory access and directory-wide export/extraction
- Launching native processes, Finder, Explorer, or another NeoTool
- Package-aware Write to INI, because the INI and all companion payloads must
  be updated as one transaction
- NeoERF multi-resource extraction to a directory

The browser build is single-threaded and may be constrained by browser memory.
Use the desktop application for full filesystem and installer-package workflows.
EOF

app_source_url="${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-vrifftech/$APP_NAME}"
shared_source_url="${GITHUB_SERVER_URL:-https://github.com}/vrifftech/NeoShared"
cat > "$DIST_DIR/CORRESPONDING_SOURCE.txt" <<EOF
Application source: $app_source_url
Application revision: $app_revision
neoshared source: $shared_source_url
neoshared revision: $shared_revision
wxWidgets-WASM source: https://github.com/PCBJam/wxWidgets
wxWidgets-WASM revision: $wx_revision
Expected wxWidgets-WASM revision: bca69b9fddc88adec57b05e6809467ef9f5158c8
Emscripten SDK: https://github.com/emscripten-core/emsdk tag 4.0.2
Build instructions: docs/BUILDING_WASM.md in the application and neoshared repositories
EOF

cat > "$DIST_DIR/404.html" <<'EOF'
<!doctype html><meta charset="utf-8"><title>Not found</title><script>location.replace('./');</script>
EOF
(
  cd "$DIST_DIR"
  find . -maxdepth 1 -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt
)
