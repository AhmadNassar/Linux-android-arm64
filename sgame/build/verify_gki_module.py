#!/usr/bin/env python3
"""Verify real GKI exports, runtime Swap symbols, configuration and metadata."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    artifacts, kernel, clang = map(pathlib.Path, sys.argv[1:4])
    target, page_size = sys.argv[4:6]
    version, generation = sys.argv[6:8]
    android = int(sys.argv[8]) if len(sys.argv) > 8 else 16
    output = (kernel / 'bazel-bin/common' / target).resolve()
    module = artifacts / f'lsdriver-swap-{version}-A{android}-{generation}-{page_size}.ko'
    readelf = str(clang / 'bin/llvm-readelf')
    symbols = subprocess.check_output([readelf, '--symbols', str(module)], text=True)
    imports = sorted({line.split()[-1] for line in symbols.splitlines()
                      if ' UND ' in line and len(line.split()) >= 8})
    exports = {line.split()[1] for line in (output / 'Module.symvers').read_text().splitlines() if line.strip()}
    missing = sorted(set(imports) - exports)
    if missing:
        raise SystemExit('Module imports missing from kernel exports: ' + ', '.join(missing))
    required = {'get_swap_device', 'swap_duplicate', 'swap_cache_get_folio', 'read_swap_cache_async',
                'kernel_read', 'vfs_getattr'}
    if tuple(map(int, version.split('.')[:2])) < (6, 0):
        required.remove('swap_cache_get_folio')
        required.update(('lookup_swap_cache', '__lock_page_killable'))
        if '__cfi_check' not in symbols:
            raise SystemExit('Legacy module CFI entry is missing')
    releases = {'swap_free', 'swap_free_nr', 'free_swap_and_cache_nr', 'free_swap_and_cache'}
    config = (output / '.config').read_text()
    if tuple(map(int, version.split('.')[:2])) >= (6, 18):
        required.update(('syscall_trace_exit', 'do_el0_softstep'))
        if 'CONFIG_UPROBES=y' in config:
            required.add('uprobe_single_step_handler')
    lookups = {line.split()[-1]: line for line in (output / 'System.map').read_text().splitlines()
               if line.split() and line.split()[-1] in required | releases}
    if required - lookups.keys() or not releases & lookups.keys():
        raise SystemExit('Kernel lacks required Swap/environment runtime symbols')
    expected_page = 'CONFIG_ARM64_16K_PAGES=y' if page_size == '16k' else 'CONFIG_ARM64_4K_PAGES=y'
    if expected_page not in config or 'CONFIG_SWAP=y' not in config:
        raise SystemExit('Wrong page size or missing CONFIG_SWAP')
    source = json.loads((artifacts / 'driver-source.json').read_text())
    layer = source['layer']
    sim = layer['simulation']
    metadata = subprocess.check_output([readelf, '--string-dump=.modinfo', str(module)], text=True)
    release = re.search(r'vermagic=(\S+)', metadata).group(1)
    if not re.match(re.escape(f'{version}-android{android}-{generation}') + r'(?:-|$)', release):
        raise SystemExit('Wrong kernel or KMI generation in module: ' + release)
    constants = '\n'.join((kernel / 'common' / name).read_text()
                          for name in ('build.config.constants', 'build.config.common'))
    if not re.search(rf'^KMI_GENERATION\s*=\s*["\x27]?{re.escape(generation)}["\x27]?\s*$', constants, re.M):
        raise SystemExit('Kernel source has a different KMI generation')
    for expected in [f'sgame_swap={layer["version"]}',
                     f'sgame_read_mode={layer["read_mode"]["name"]}',
                     f'sgame_simulation={sim["version"]}', f'sgame_simulation_source=enkernel-{sim["commit"]}']:
        if expected not in metadata:
            raise SystemExit('Missing module metadata: ' + expected)
    for expected in ('sgame_core=author-features-group-poll', 'sgame_network_probe=disabled',
                     'sgame_features=module-hide,thread-hide,client-hide,kgsl-hide,proc-block,exit-hooks',
                     'sgame_cpu_render=disabled-upstream'):
        if expected not in metadata:
            raise SystemExit('Missing restored feature metadata: ' + expected)
    (artifacts / 'module-info.txt').write_text(metadata)
    (artifacts / 'module-imports.txt').write_text('\n'.join(imports) + '\n')
    (artifacts / 'lookup-symbols.json').write_text(json.dumps(lookups, indent=2) + '\n')
    (artifacts / 'kernel.config').write_text(config)
    record = {
        'kernel': version, 'android': android, 'kmi': f'android{android}-{generation}',
        'kmi_generation': int(generation), 'kernel_release': release,
        'target': target, 'page_size': page_size,
        'common_commit': subprocess.check_output(['git', '-C', str(kernel / 'common'), 'rev-parse', 'HEAD'], text=True).strip(),
        'read_mode': layer['read_mode']['name'],
        'source_commit': source['commit'],
        'swap_version': layer['version'], 'simulation_version': sim['version'],
        'simulation_source_commit': sim['commit'], 'module': module.name,
        'sha256': sha(module), 'size': module.stat().st_size,
        'imports_checked': len(imports), 'missing_exports': missing,
        'runtime_lookup_symbols_checked': sorted(lookups),
        'runtime_release_symbols_available': sorted(releases & lookups.keys()),
        'driver_source_sha256': sha(artifacts / 'driver-source.json'),
        'kernel_manifest_sha256': sha(artifacts / 'kernel-manifest.xml'),
        'kernel_config_sha256': sha(artifacts / 'kernel.config'),
        'compiler': subprocess.check_output([str(clang / 'bin/clang'), '--version'], text=True),
        'device_validation': 'pending',
    }
    (artifacts / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)


if __name__ == '__main__':
    main()
