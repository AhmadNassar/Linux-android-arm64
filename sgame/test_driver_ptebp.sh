#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir/installer.h" "${PTEBP_SOURCE:-lsdriver/arm64_ptedbg-20260822-000439.h}" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[2]).read_text()
def function(name):
    start = text.index('static ', text.rfind('\n}', 0, text.index(name + '(')) + 2)
    body = text.index('{', text.index(name + '(', start))
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
Path(sys.argv[1]).write_text('\n\n'.join(function(n) for n in (
    'ptebp_drop_all_monitors', 'ptebp_stop_locked', 'start_ptebp_monitor')) + '\n')
PY
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DPTEBP_PRODUCTION=\""$test_dir/installer.h"\" \
    sgame/tests/driver_ptebp_install_test.c -o "$test_dir/installer"
"$test_dir/installer"
