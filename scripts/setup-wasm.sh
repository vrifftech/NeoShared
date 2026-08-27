#!/usr/bin/env bash
set -euo pipefail

EMSCRIPTEN_VERSION="4.0.2"
WX_WASM_REPOSITORY="https://github.com/PCBJam/wxWidgets.git"
WX_WASM_COMMIT="bca69b9fddc88adec57b05e6809467ef9f5158c8"
DEPS_ROOT=""
FORCE=0
INSTALL_WX=1

usage() {
  cat <<'USAGE'
usage: setup-wasm.sh --deps-root DIR [--force] [--skip-wx]

Installs the pinned Emscripten SDK and, unless --skip-wx is supplied, checks
out the pinned PCBJam/wxWidgets wasm-port commit under DIR. Nothing is
installed globally.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    --force) FORCE=1; shift;;
    --skip-wx) INSTALL_WX=0; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done
[[ -n "$DEPS_ROOT" ]] || { echo "--deps-root is required" >&2; exit 2; }
for cmd in git python3; do command -v "$cmd" >/dev/null || { echo "Missing command: $cmd" >&2; exit 2; }; done
mkdir -p "$DEPS_ROOT"
DEPS_ROOT="$(cd "$DEPS_ROOT" && pwd)"
EMSDK_ROOT="$DEPS_ROOT/emsdk"
WX_SOURCE="$DEPS_ROOT/wxwidgets-wasm"

if [[ "$FORCE" == 1 ]]; then
  rm -rf "$EMSDK_ROOT"
  [[ "$INSTALL_WX" == 0 ]] || rm -rf "$WX_SOURCE"
fi

if [[ ! -d "$EMSDK_ROOT/.git" ]]; then
  git clone --depth 1 --branch "$EMSCRIPTEN_VERSION" https://github.com/emscripten-core/emsdk.git "$EMSDK_ROOT"
fi
"$EMSDK_ROOT/emsdk" install "$EMSCRIPTEN_VERSION"
"$EMSDK_ROOT/emsdk" activate "$EMSCRIPTEN_VERSION"

if [[ "$INSTALL_WX" == 1 ]]; then
  if [[ ! -d "$WX_SOURCE/.git" ]]; then
    git init "$WX_SOURCE"
    git -C "$WX_SOURCE" remote add origin "$WX_WASM_REPOSITORY"
  fi
  CURRENT_WX="$(git -C "$WX_SOURCE" rev-parse HEAD 2>/dev/null || true)"
  if [[ "$CURRENT_WX" != "$WX_WASM_COMMIT" ]]; then
    git -C "$WX_SOURCE" fetch --depth 1 origin "$WX_WASM_COMMIT"
    git -C "$WX_SOURCE" checkout --detach FETCH_HEAD
  fi
  git -C "$WX_SOURCE" submodule update --init --recursive
fi

cat > "$DEPS_ROOT/neo-wasm-versions.env" <<EOF_ENV
export NEO_WASM_EMSCRIPTEN_VERSION='$EMSCRIPTEN_VERSION'
export NEO_WASM_WX_COMMIT='$WX_WASM_COMMIT'
export NEO_WASM_EMSDK_ROOT='$EMSDK_ROOT'
export NEO_WASM_WX_SOURCE='$WX_SOURCE'
EOF_ENV
printf 'Emscripten: %s\n' "$EMSCRIPTEN_VERSION"
if [[ "$INSTALL_WX" == 1 ]]; then
  printf 'wxWidgets-WASM: %s\n' "$WX_WASM_COMMIT"
fi
