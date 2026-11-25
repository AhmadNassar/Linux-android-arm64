#!/usr/bin/env python3
"""Audit preserved upstream features and the no-overlay source contract."""
import hashlib
import json
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parent.parent
record = json.loads((ROOT / 'sgame/upstream.json').read_text())
for name, expected in record['preserved_upstream_sha256'].items():
    assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == expected, name

def code(path):
    text = (ROOT / path).read_text()
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
main = code('lsdriver/lsdriver.c')
session = code('lsdriver/sgame_session.h')
init = code('lsdriver/sgame_lifecycle.h')
assert not re.search(r'\bipv4_ping\w*\s*\(', main + init)
assert not re.search(r'^\s*#if\s+0\b', main + init + session, re.M)
for name in ('hide_myself', 'proc_create_block_init', 'do_exit_init', 'bypass_cfi'):
    assert re.search(r'\b' + name + r'\s*\(', init), name
assert not re.search(r'\bcpu_render_(?:init|exit)\s*\(', main + init)
assert 'CpuRenderTestThreadFunction' not in main + init
assert 'MODULE_INFO(sgame_cpu_render, "disabled-upstream")' in init
for name in ('hide_task_install', 'hide_kgsl_install', 'hide_task_remove', 'hide_kgsl_remove'):
    assert re.search(r'\b' + name + r'\s*\(', session), name
assert init.index('do_exit_init()') < init.index('hide_myself()')
assert session.index('hide_kgsl_install(task->tgid)') < session.index('__atomic_store_n(&mapping->user, 1')
assert 'HOOK_ENTRY("taskstats_exit", taskstats_exit_hook_work)' in main
for symbol in ('arm64_force_sig_fault', 'do_group_exit', 'do_exit'):
    assert 'HOOK_ENTRY("' + symbol + '"' in main, symbol
assert '-march=armv8.6' not in code('lsdriver/Makefile')
assert 'kernel_render/cpu/cpu_render.o' in code('lsdriver/Makefile')
assert 'SGAME_READ_NC ?= 0' in code('lsdriver/Makefile')
assert 'SGAME_UNAVAILABLE_PTEDBG_H' not in code('lsdriver/arm64_ptedbg.h')
breakpoints = code('lsdriver/break_point.h')
assert '#include "arm64_ptedbg-20260822-000439.h"' in breakpoints
assert '#include "arm64_ptedbg.h"' not in breakpoints
assert 'READ_ONCE(g_ptebp_info)' in breakpoints
assert 'git apply' not in (ROOT/'sgame/export_source.py').read_text()
for name in ('build-read-matrix.sh', 'build-5.10.sh', 'build_5_10.py', '5.10-baselines.json',
             'build-6.1.138.sh', 'build-6.12.sh',
             'build-6.12.23.sh', 'build-6.12.58.sh', 'prepare_host_tools.py',
             'verify_gki_module.py', 'verify_pac_scs.py', 'kernel-baselines.json',
             '6.12.23-manifest.xml', '6.12.58-manifest.xml'):
    assert (ROOT/'sgame/build'/name).is_file(), name
for name in ('build-gki.sh', 'build_nc_matrix.py', '5.15.206-manifest.xml',
             '6.6.89-manifest.xml', '6.18.21-manifest.xml'):
    assert (ROOT/'sgame/build'/name).is_file(), name
print('PASS: author interfaces/native PTE and project extensions retained; upstream CPU rendering paused; network probe disabled; committed-source builds')
