#!/usr/bin/env bash
# Reuse three pinned kernel workspaces; build both read modes from recorded sources.
set -euo pipefail
WORK61="${1:?6.1 workspace}"
WORK5="${2:?A16-5 workspace}"
WORK6="${3:?A16-6 workspace}"
CACHED="$(realpath "${4:?prepared cached source}")"
NC="$(realpath "${5:?prepared uncached source}")"
DEST="$(realpath -m "${6:?new output directory}")"
ADAPTER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$DEST/sources"
for kernel in ${DRIVER_KERNELS:-6.1.138 6.12.23 6.12.58}; do
    for mode in cached nc; do
        prepared="$CACHED"
        [[ "$mode" != nc ]] || prepared="$NC"
        source="$DEST/sources/$kernel-$mode"
        mkdir -p "$source"
        cp -a "$prepared/." "$source/"
        case "$kernel" in
            6.1.138) work="$WORK61"; artifacts="$work/artifacts" ;;
            6.12.23) work="$WORK5"; artifacts="$work/artifacts/4k" ;;
            6.12.58) work="$WORK6"; artifacts="$work/artifacts/4k" ;;
        esac
        result="$DEST/$kernel/$mode"
        mkdir -p "$result"
        bash "$ADAPTER/build-$kernel.sh" "$work" "$source" > "$result/build.log" 2>&1
        python3 - "$artifacts" "$result" <<'PY'
import json, pathlib, shutil, sys
source, result = map(pathlib.Path, sys.argv[1:])
module = json.loads((source / 'build.json').read_text())['module']
for name in ('build.json', 'driver-source.json', 'host-tools.json', 'kernel-manifest.xml',
             'kernel.config', 'lookup-symbols.json', 'module-imports.txt', 'module-info.txt', module):
    shutil.copy2(source / name, result / name)
if (source / 'pac-scs.json').exists() and module == 'lsdriver-swap-6.1.138.ko':
    shutil.copy2(source / 'pac-scs.json', result / 'pac-scs.json')
PY
    done
done
