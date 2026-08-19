#!/usr/bin/env bash
set -euo pipefail
unset AUTOM4TE
exec autom4te --no-cache "$@"
