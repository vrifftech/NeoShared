#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSIONS_FILE="$SCRIPT_DIR/../wasm/versions.env"
[[ -f "$VERSIONS_FILE" ]] || { echo "Missing WebAssembly version manifest: $VERSIONS_FILE" >&2; exit 2; }
# shellcheck disable=SC1090
source "$VERSIONS_FILE"
EMSCRIPTEN_VERSION="$NEO_WASM_EMSCRIPTEN_VERSION"
WX_WASM_REPOSITORY="$NEO_WASM_WX_REPOSITORY"
WX_WASM_COMMIT="$NEO_WASM_WX_COMMIT"
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
# shellcheck disable=SC1091
source "$EMSDK_ROOT/emsdk_env.sh" >/dev/null
EMCC_VERSION_OUTPUT="$(emcc --version)"
grep -F "$EMSCRIPTEN_VERSION" <<<"$EMCC_VERSION_OUTPUT" >/dev/null || {
  echo "Activated emcc does not match the pinned version $EMSCRIPTEN_VERSION" >&2
  exit 2
}

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
  [[ "$(git -C "$WX_SOURCE" rev-parse HEAD)" == "$WX_WASM_COMMIT" ]] || {
    echo "wxWidgets checkout does not match the pinned commit $WX_WASM_COMMIT" >&2
    exit 2
  }
fi

cat > "$DEPS_ROOT/neo-wasm-versions.env" <<EOF_ENV
export NEO_WASM_EMSCRIPTEN_VERSION='$EMSCRIPTEN_VERSION'
export NEO_WASM_WX_COMMIT='$WX_WASM_COMMIT'
export NEO_WASM_WX_REPOSITORY='$WX_WASM_REPOSITORY'
export NEO_WASM_BUILD_MODEL='$NEO_WASM_BUILD_MODEL'
export NEO_WASM_VERSION_MANIFEST='$VERSIONS_FILE'
export NEO_WASM_EMSDK_ROOT='$EMSDK_ROOT'
export NEO_WASM_WX_SOURCE='$WX_SOURCE'
EOF_ENV
printf 'Emscripten: %s\n' "$EMSCRIPTEN_VERSION"
if [[ "$INSTALL_WX" == 1 ]]; then
  printf 'wxWidgets-WASM: %s\n' "$WX_WASM_COMMIT"
fi
