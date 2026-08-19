#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR=""
DIST_DIR=""
APP_NAME=""
SLUG=""
VERSION=""
ICON=""

usage() {
  cat <<'USAGE'
usage: package-wasm-site.sh --input DIR --dist DIR --app-name NAME --slug SLUG --version VERSION --icon FILE
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
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

for value in INPUT_DIR DIST_DIR APP_NAME SLUG VERSION ICON; do
  [[ -n "${!value}" ]] || { echo "Missing required option: $value" >&2; exit 2; }
done

HTML="$INPUT_DIR/$SLUG.html"
[[ -f "$HTML" ]] || { echo "Missing Emscripten HTML output: $HTML" >&2; exit 1; }
[[ -f "$INPUT_DIR/$SLUG.js" ]] || { echo "Missing Emscripten JavaScript output" >&2; exit 1; }
[[ -f "$INPUT_DIR/$SLUG.wasm" ]] || { echo "Missing WebAssembly output" >&2; exit 1; }
[[ -f "$ICON" ]] || { echo "Missing application icon: $ICON" >&2; exit 1; }

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
cp "$HTML" "$DIST_DIR/index.html"
find "$INPUT_DIR" -maxdepth 1 -type f -name "$SLUG.*" ! -name "$SLUG.html" -exec cp {} "$DIST_DIR/" \;
cp "$ICON" "$DIST_DIR/$SLUG.svg"
cp "$ICON" "$DIST_DIR/favicon.svg"
: > "$DIST_DIR/.nojekyll"

cat > "$DIST_DIR/site.webmanifest" <<EOF_MANIFEST
{
  "name": "$APP_NAME",
  "short_name": "$APP_NAME",
  "start_url": "./",
  "display": "standalone",
  "background_color": "#20242b",
  "theme_color": "#20242b",
  "icons": [{"src":"$SLUG.svg","sizes":"any","type":"image/svg+xml"}]
}
EOF_MANIFEST

cat > "$DIST_DIR/THIRD_PARTY_NOTICES.txt" <<EOF_NOTICES
$APP_NAME $VERSION WebAssembly build

wxWidgets-WASM
Source: https://github.com/PCBJam/wxWidgets
Pinned commit: bca69b9fddc88adec57b05e6809467ef9f5158c8
The WebAssembly-specific wxWidgets port sources identify LGPL-2.0 terms.
The remainder of wxWidgets retains its upstream licensing terms.

Emscripten SDK
Source: https://github.com/emscripten-core/emsdk
Version: 4.0.2
EOF_NOTICES

cat > "$DIST_DIR/404.html" <<'EOF_404'
<!doctype html><meta charset="utf-8"><title>Not found</title><script>location.replace('./');</script>
EOF_404
