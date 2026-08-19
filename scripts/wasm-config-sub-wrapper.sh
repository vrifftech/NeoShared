#!/usr/bin/env bash
set -euo pipefail

# Autoconf invokes CONFIG_SHELL/SHELL both as a shell and as the interpreter
# for package-local config.sub copies. Intercept only config.sub and delegate
# every other invocation to bash. Modern GNU config.sub recognizes the canonical
# wasm32-unknown-emscripten triple but not wxWidgets' historical shorthand
# --host=emscripten, so normalize that one argument before delegation.
if [[ "${1:-}" == *config.sub ]]; then
  [[ -n "${NEO_WASM_CONFIG_SUB:-}" && -x "$NEO_WASM_CONFIG_SUB" ]] || {
    echo "NEO_WASM_CONFIG_SUB does not identify an executable GNU config.sub" >&2
    exit 2
  }
  shift
  normalized=()
  for argument in "$@"; do
    if [[ "$argument" == "emscripten" ]]; then
      normalized+=("wasm32-emscripten")
    else
      normalized+=("$argument")
    fi
  done
  exec "$NEO_WASM_CONFIG_SUB" "${normalized[@]}"
fi
exec /bin/bash "$@"
