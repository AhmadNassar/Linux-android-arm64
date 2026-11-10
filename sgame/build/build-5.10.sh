#!/usr/bin/env bash
# Build either pinned Android 5.10 GKI baseline from a committed driver export.
set -euo pipefail
exec python3 "$(dirname "${BASH_SOURCE[0]}")/build_5_10.py" "$@"
