#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir/dispatch.h" <<'PY'
from pathlib import Path
import sys
text = Path('lsdriver/arm64_stepdbg.h').read_text()
def function(name):
    position = text.index(name + '(')
    start = text.rfind('\nstatic ', 0, position) + 1
    body = text.index('{', position)
    end, depth = body + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
array = text.index('static struct hook_entry g_stepbp_required_hooks[]')
array_end = text.index('\n};', array) + 3
sections = [function(name) for name in ('stepbp_replayed_pc_locked',
    'stepbp_finish_call_step_hook', 'stepbp_user_single_step',
    'work_trampoline_stepbp_el0_softstep')]
sections.append(text[array:array_end])
sections.extend(function(name) for name in ('stepbp_install_required_hooks', 'stepbp_remove_required_hooks'))
Path(sys.argv[1]).write_text('\n\n'.join(sections) + '\n')
PY
for uprobes in 0 1; do
    "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
        -DCONFIG_UPROBES="$uprobes" -DSTEPBP_PRODUCTION=\""$test_dir/dispatch.h"\" \
        sgame/tests/driver_stepbp_dispatch_test.c -o "$test_dir/dispatch-$uprobes"
    "$test_dir/dispatch-$uprobes"
done

