#!/usr/bin/env bash
# Android16 KMI 6, kept separate from the KMI 5 module.
set -euo pipefail
ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$ADAPTER_DIR/build-6.12.sh" "${1:?work root required}" \
    "${2:?prepared driver source required}" a16-6 "${3:-4k}"
