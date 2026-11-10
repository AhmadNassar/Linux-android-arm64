#!/usr/bin/env python3
"""Build and audit the page-based 5.10 driver with its original GKI CFI/LTO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path)
    parser.add_argument('source', type=Path)
    parser.add_argument('variant', choices=('a12', 'a13'))
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--clang', required=True, type=Path)
    parser.add_argument('--host-tools', type=Path)
    parser.add_argument('--jobs', type=int, default=16)
    args = parser.parse_args()
    baseline = json.loads((HERE / '5.10-baselines.json').read_text())[args.variant]
    work, source, kernel, clang = (p.resolve() for p in
                                   (args.work, args.source, args.kernel, args.clang))
    record = json.loads((source / 'sgame-source.json').read_text())
    for name, expected in record['files_sha256'].items():
        if sha(source / name) != expected:
            raise SystemExit('changed committed input: ' + name)
    if output('git', '-C', kernel, 'rev-parse', 'HEAD') != baseline['common_commit']:
        raise SystemExit('wrong kernel revision')
    if output('git', '-C', clang, 'rev-parse', 'HEAD') != baseline['clang_commit']:
        raise SystemExit('wrong Clang revision')
    for name, expected in baseline['reference_sha256'].items():
        if sha(kernel / name) != expected:
            raise SystemExit('changed kernel API input: ' + name)
    mode = record['layer']['read_mode']['name']
    artifacts = work / 'artifacts' / mode
    artifacts.mkdir(parents=True, exist_ok=True)
    out = work / 'out'
    env = dict(os.environ)
    host_bin = str(args.host_tools.resolve() / 'linux-x86/bin') + ':' if args.host_tools else ''
    env.update(PATH=str(clang / 'bin') + ':' + host_bin + env['PATH'],
               BRANCH=baseline['branch'], KMI_GENERATION=str(baseline['kmi_generation']),
               KBUILD_BUILD_USER='sgame', KBUILD_BUILD_HOST='gki-build',
               KBUILD_BUILD_TIMESTAMP='2026-11-10 00:00:00 UTC')
    make = ['make', '-C', str(kernel), 'O=' + str(out), 'ARCH=arm64',
            'LLVM=1', 'LLVM_IAS=1', 'CROSS_COMPILE=aarch64-linux-gnu-',
            'HOSTCC=gcc', 'HOSTCXX=g++', 'KCFLAGS=-D__ANDROID_COMMON_KERNEL__',
            '-j' + str(args.jobs)]
    if not (out / '.config').exists():
        run(*make, 'gki_defconfig', env=env)
    config = (out / '.config').read_text()
    for option in ('CONFIG_ARM64_4K_PAGES=y', 'CONFIG_SWAP=y', 'CONFIG_CFI_CLANG=y'):
        if option not in config:
            raise SystemExit('required GKI option missing: ' + option)
    if not (out / 'Module.symvers').exists() or not (out / 'System.map').exists():
        run(*make, 'vmlinux', 'modules', env=env)
    run(*make, 'modules_prepare', env=env)
    # Preserve the upstream empty-CRC delivery convention. Verify every actual
    # import against the real symbol table after linking, and always restore it.
    symvers = out / 'Module.symvers'
    exports = {line.split()[1] for line in symvers.read_text().splitlines() if line.strip()}
    saved = out / 'Module.symvers.sgame-5.10'
    if saved.exists():
        raise SystemExit('another module build owns this kernel output')
    symvers.rename(saved)
    try:
        run(*make, 'M=' + str(source / 'lsdriver'),
            'SGAME_READ_NC=' + str(int(mode == 'nc')), 'KBUILD_MODPOST_WARN=1',
            'CONFIG_DEBUG_INFO_BTF_MODULES=', 'modules', env=env)
    finally:
        saved.rename(symvers)
    name = 'lsdriver-swap-5.10.237-A' + str(baseline['android']) + '-' + \
           str(baseline['kmi_generation']) + '-4k.ko'
    module = artifacts / name
    shutil.copy2(source / 'lsdriver/lsdriver.ko', module)
    symbols = output(clang / 'bin/llvm-readelf', '--symbols', module)
    imports = sorted({line.split()[-1] for line in symbols.splitlines()
                      if ' UND ' in line and len(line.split()) >= 8})
    missing = sorted(set(imports) - exports)
    if missing:
        raise SystemExit('module imports absent from GKI exports: ' + ', '.join(missing))
    if '__cfi_check' not in symbols:
        raise SystemExit('legacy module CFI entry is missing')
    metadata = output(clang / 'bin/llvm-readelf', '--string-dump=.modinfo', module)
    layer = record['layer']
    required_info = [
        'vermagic=5.10.237-android' + str(baseline['android']) + '-' + str(baseline['kmi_generation']),
        'sgame_swap=' + layer['version'], 'sgame_read_mode=' + mode,
        'sgame_simulation=' + layer['simulation']['version'],
        'sgame_simulation_source=enkernel-' + layer['simulation']['commit'],
        'sgame_core=author-features-group-poll', 'sgame_network_probe=disabled',
        'sgame_features=' + ','.join(layer['features'])]
    for item in required_info:
        if item not in metadata:
            raise SystemExit('wrong module metadata: ' + item)
    required = {'get_swap_device', 'swap_duplicate', 'lookup_swap_cache',
                'read_swap_cache_async', 'kernel_read', 'vfs_getattr'}
    releases = {'swap_free', 'swap_free_nr', 'free_swap_and_cache_nr', 'free_swap_and_cache'}
    lookups = {line.split()[-1]: line for line in (out / 'System.map').read_text().splitlines()
               if line.split() and line.split()[-1] in required | releases}
    if required - lookups.keys() or not releases & lookups.keys():
        raise SystemExit('required runtime lookup missing: ' + ', '.join(sorted(required - lookups.keys())))
    for path, expected in record['files_sha256'].items():
        if sha(source / path) != expected:
            raise SystemExit('build changed committed source: ' + path)
    shutil.copy2(source / 'sgame-source.json', artifacts / 'driver-source.json')
    shutil.copy2(out / '.config', artifacts / 'kernel.config')
    shutil.copy2(symvers, artifacts / 'Module.symvers')
    shutil.copy2(out / 'System.map', artifacts / 'System.map')
    (artifacts / 'module-info.txt').write_text(metadata + '\n')
    (artifacts / 'module-imports.txt').write_text('\n'.join(imports) + '\n')
    (artifacts / 'lookup-symbols.json').write_text(json.dumps(lookups, indent=2) + '\n')
    result = dict(kernel='5.10.237', android=baseline['android'],
                  kmi_generation=baseline['kmi_generation'], page_size='4k',
                  common_commit=baseline['common_commit'], clang_commit=baseline['clang_commit'],
                  compiler=output(clang / 'bin/clang', '--version'),
                  source_commit=record['commit'], swap_version=layer['version'],
                  simulation_version=layer['simulation']['version'], read_mode=mode,
                  module=name, sha256=sha(module), size=module.stat().st_size,
                  imports_checked=len(imports), missing_exports=missing,
                  runtime_lookup_symbols_checked=sorted(lookups), legacy_cfi_entry=True,
                  source_files_checked=len(record['files_sha256']), source_changed_by_build=False,
                  kernel_config_sha256=sha(artifacts / 'kernel.config'),
                  driver_source_sha256=sha(artifacts / 'driver-source.json'),
                  build_script_sha256=sha(Path(__file__)), device_validation='pending')
    (artifacts / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
