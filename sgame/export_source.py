#!/usr/bin/env python3
"""Export one committed driver revision and record its build inputs; no patches or source rewrites."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])

def export(output, read_mode='cached', revision='HEAD'):
    commit = git('rev-parse', '--verify', revision + '^{commit}').decode().strip()
    names = git('ls-tree', '-r', '--name-only', commit).decode().splitlines()
    def wanted(name):
        if name in ('build_all.sh', 'packer.sh', 'LICENSE'):
            return True
        if not name.startswith(('lsdriver/', 'android/jni/driver/', 'android/jni/logger/', 'sgame/')):
            return False
        if name.startswith(('lsdriver/byte_stream/', 'lsdriver/arm64_tests/')):
            return False
        return Path(name).suffix in ('.c', '.cpp', '.h', '.hpp', '.S', '.sh', '.py', '.json', '.md', '.xml', '.txt') or Path(name).name in ('Makefile', 'Kbuild')
    paths = [name for name in names if wanted(name)]
    required = ('sgame/build/build-read-matrix.sh', 'sgame/build/build-5.10.sh',
                'sgame/build/build_5_10.py', 'sgame/build/5.10-baselines.json',
                'sgame/build/build-6.1.138.sh',
                'sgame/build/build-6.12.sh', 'sgame/build/kernel-baselines.json',
                'sgame/tests/driver_simulation_client_test.cpp',
                'sgame/tests/driver_simulation_abi_compile.cpp')
    missing = [name for name in required if name not in paths]
    if missing:
        raise SystemExit('Committed driver source lacks required build inputs: ' + ', '.join(missing))
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    payload = git('archive', '--format=tar', commit, '--', *paths)
    subprocess.run(['tar', '-xf', '-', '-C', str(output)], input=payload, check=True)
    def sha(path):
        return hashlib.sha256((output / path).read_bytes()).hexdigest()
    protocol = ['android/jni/driver/driver.h', 'android/jni/driver/driver_loader.h',
                'lsdriver/io_version/io_struct.h', 'lsdriver/io_version/version.h']
    simulation = json.loads((output / 'lsdriver/sgame_simulation/source.json').read_text())
    version = re.search(r'#define SGAME_SWAP_VERSION "([^"]+)"', (output / 'lsdriver/sgame_swap_read.h').read_text())[1]
    source = {
        'source_kind': 'committed-git-source',
        'repository': 'https://github.com/AhmadNassar/Linux-android-arm64',
        'branch': 'sgame', 'commit': commit,
        'protocol_sha256': {name: sha(name) for name in protocol},
        'files_sha256': {name: sha(name) for name in paths},
        'layer': {'version': version, 'sha256': sha('lsdriver/sgame_swap_read.h'),
                  'read_mode': {'name': read_mode, 'make_variable': 'SGAME_READ_NC', 'make_value': int(read_mode == 'nc')},
                  'simulation': {'version': simulation['version'], 'commit': simulation['commit']},
                  'features': ['module-hide', 'thread-hide', 'client-hide', 'kgsl-hide', 'proc-block', 'exit-hooks', 'cpu-render'],
                  'network_probe': False},
    }
    (output / 'sgame-source.json').write_text(json.dumps(source, indent=2) + '\n')
    return source

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--read-mode', choices=('cached', 'nc'), default='cached')
    parser.add_argument('--revision', default='HEAD')
    args = parser.parse_args()
    result = export(args.output, args.read_mode, args.revision)
    print(f"Exported {result['commit']} read_mode={args.read_mode} to {args.output}")
