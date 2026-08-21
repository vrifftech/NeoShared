#!/usr/bin/env bash
set -euo pipefail

DEPS_ROOT=""
JOBS=""
CLEAN=0

usage() {
  cat <<'USAGE'
usage: build-wx-wasm.sh --deps-root DIR [--jobs N] [--clean]

Builds the pinned PCBJam wxWidgets DOM/WebAssembly port as static libraries.
The build intentionally uses Emscripten's JavaScript exception model and no
pthreads, allowing deployment on ordinary GitHub Pages without COOP/COEP.
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done
[[ -n "$DEPS_ROOT" ]] || { echo "--deps-root is required" >&2; exit 2; }
DEPS_ROOT="$(cd "$DEPS_ROOT" && pwd)"
[[ -f "$DEPS_ROOT/neo-wasm-versions.env" ]] || { echo "Run setup-wasm.sh first" >&2; exit 2; }
# shellcheck disable=SC1090
source "$DEPS_ROOT/neo-wasm-versions.env"
# shellcheck disable=SC1090
source "$NEO_WASM_EMSDK_ROOT/emsdk_env.sh" >/dev/null
WX_SOURCE="$NEO_WASM_WX_SOURCE"
WX_BUILD="$DEPS_ROOT/wxwidgets-wasm-build"
[[ -n "$JOBS" ]] || JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
[[ "$CLEAN" == 0 ]] || rm -rf "$WX_BUILD"

for cmd in emcc em++ emconfigure emmake emar embuilder autoconf autoreconf automake make install; do
  command -v "$cmd" >/dev/null || { echo "Missing command: $cmd" >&2; exit 2; }
done

GNU_CONFIG_DIR="$(automake --print-libdir)"
NEO_WASM_CONFIG_SUB="$GNU_CONFIG_DIR/config.sub"
[[ -x "$NEO_WASM_CONFIG_SUB" ]] || { echo "GNU config.sub was not found: $NEO_WASM_CONFIG_SUB" >&2; exit 2; }
"$NEO_WASM_CONFIG_SUB" wasm32-emscripten >/dev/null || {
  echo "The installed GNU config.sub does not recognize a wasm32 Emscripten host." >&2
  exit 2
}
export NEO_WASM_CONFIG_SUB
# Autoconf executes AUTOM4TE, SHELL, and CONFIG_SHELL directly. Git's
# executable bit can be lost when a repository is populated from a ZIP or
# committed from a filesystem that does not preserve Unix modes. Install
# private executable copies in the dependency cache instead of relying on the
# mode of the checked-out neoshared scripts.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WRAPPER_DIR="$DEPS_ROOT/autoconf-wrappers"
mkdir -p "$WRAPPER_DIR"
install -m 0755 \
  "$SCRIPT_DIR/wasm-config-sub-wrapper.sh" \
  "$WRAPPER_DIR/wasm-config-sub-wrapper.sh"
install -m 0755 \
  "$SCRIPT_DIR/wasm-autom4te-wrapper.sh" \
  "$WRAPPER_DIR/wasm-autom4te-wrapper.sh"
export SHELL="$WRAPPER_DIR/wasm-config-sub-wrapper.sh"
export CONFIG_SHELL="$WRAPPER_DIR/wasm-config-sub-wrapper.sh"
export AUTOM4TE="$WRAPPER_DIR/wasm-autom4te-wrapper.sh"

MARKER_TEXT="emscripten=$NEO_WASM_EMSCRIPTEN_VERSION wx=$NEO_WASM_WX_COMMIT model=js-exceptions-no-pthreads-v3"
if [[ -x "$WX_BUILD/wx-config" && -f "$WX_BUILD/.neo-wasm-build" ]] &&
   [[ "$(cat "$WX_BUILD/.neo-wasm-build")" == "$MARKER_TEXT" ]]; then
  echo "wxWidgets-WASM is already built at $WX_BUILD"
  exit 0
fi

rm -rf "$WX_BUILD"
mkdir -p "$WX_BUILD"
embuilder build zlib

if [[ ! -x "$WX_SOURCE/configure" || "$WX_SOURCE/configure.in" -nt "$WX_SOURCE/configure" ||
      ( -f "$WX_SOURCE/autoconf_inc.m4" && "$WX_SOURCE/autoconf_inc.m4" -nt "$WX_SOURCE/configure" ) ]]; then
  (cd "$WX_SOURCE" && autoconf)
fi
if [[ -d "$WX_SOURCE/3rdparty/pcre" ]]; then
  (cd "$WX_SOURCE/3rdparty/pcre" && autoreconf -fi >/dev/null 2>&1 || true)
fi

EM_CACHE_SYSROOT="$(em-config CACHE)/sysroot"
PCRE2_INCLUDE="$WX_BUILD/3rdparty/pcre/src"
export CFLAGS="-O2 -DNDEBUG -fexceptions -DZ_HAVE_UNISTD_H=1 -I$EM_CACHE_SYSROOT/include"
export CXXFLAGS="-O2 -DNDEBUG -fexceptions -DZ_HAVE_UNISTD_H=1 -I$EM_CACHE_SYSROOT/include -I$PCRE2_INCLUDE"
export LDFLAGS="-fexceptions -sUSE_ZLIB=1 -L$EM_CACHE_SYSROOT/lib/wasm32-emscripten"

cd "$WX_BUILD"
emconfigure "$WX_SOURCE/configure" \
  --host=emscripten \
  --without-subdirs \
  --disable-shared \
  --with-opengl \
  --enable-exceptions \
  --disable-threads \
  --disable-richtext \
  --without-libtiff \
  --disable-xlocale \
  --with-cxx=17 \
  --enable-utf8 \
  --with-zlib=sys \
  --disable-debug

if [[ -d 3rdparty/pcre ]]; then
  emmake make -j"$JOBS" -C 3rdparty/pcre || emmake make -j1 -C 3rdparty/pcre
fi
if [[ ! -f "$(em-config CACHE)/sysroot/include/zlib.h" ]]; then
  embuilder build zlib --force
fi
if ! emmake make -j"$JOBS"; then
  echo "Parallel wxWidgets build failed; retrying serially." >&2
  emmake make -j1
fi

cd "$WX_BUILD/lib"
shopt -s nullglob
for lib in *-emscripten.a; do
  [[ -L "$lib" ]] && continue
  ln -sfn "$lib" "${lib/-emscripten/}"
done
for stub in richtext webview; do
  rm -f "libwx_wasmu_${stub}-3.2.a" "libwx_wasmu_${stub}-3.2-emscripten.a"
  emar rcs "libwx_wasmu_${stub}-3.2.a"
  ln -s "libwx_wasmu_${stub}-3.2.a" "libwx_wasmu_${stub}-3.2-emscripten.a"
done
printf '%s' "$MARKER_TEXT" > "$WX_BUILD/.neo-wasm-build"
"$WX_BUILD/wx-config" --version
