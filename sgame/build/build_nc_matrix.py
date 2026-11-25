#!/usr/bin/env python3
"""Build all eight pinned Android configurations from one committed NC export."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
PROFILES = ('a12-510', 'a13-510', 'a13-515', 'a14-61', 'a15-66', 'a16-5', 'a16-6', 'a17-618')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('work', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--profiles', nargs='+', choices=PROFILES, default=list(PROFILES))
    parser.add_argument('--jobs', type=int, default=16)
    args = parser.parse_args()
    source, work, dest = (path.resolve() for path in (args.source, args.work, args.destination))
    record = json.loads((source / 'sgame-source.json').read_text())
    if record['source_kind'] != 'committed-git-source' or record['layer']['read_mode']['name'] != 'nc':
        raise SystemExit('This matrix requires a committed NC source export')
    dest.mkdir(parents=True, exist_ok=True)
    results = json.loads((dest / 'matrix.json').read_text()) if (dest / 'matrix.json').exists() else {}
    env = dict(os.environ, JOBS=str(args.jobs), SYNC_LOCAL_ONLY='1',
               TEST_TMPDIR=str(work / 'bazel-tmp'))
    for profile in args.profiles:
        result = dest / profile / 'nc'
        result.mkdir(parents=True, exist_ok=True)
        if (result / 'build.json').exists():
            raise SystemExit('Preserve existing build results; use a new destination: ' + str(result))
        prepared = work / 'build-sources' / (profile + '-nc')
        if prepared.exists():
            raise SystemExit('Use a fresh per-profile source directory: ' + str(prepared))
        shutil.copytree(source, prepared)
        root = work / profile
        if profile in ('a12-510', 'a13-510'):
            variant = profile[:3]
            cmd = ['bash', str(HERE / 'build-5.10.sh'), str(root), str(prepared), variant,
                   '--kernel', str(work / 'deps' / variant / 'common'),
                   '--clang', str(work / 'deps/toolchains' / variant / ('clang-' + ('r416183b' if variant == 'a12' else 'r450784e'))),
                   '--host-tools', str(work / 'host-inputs/build-tools'),
                   '--kernel-host-tools', str(work / 'host-inputs/kernel-build-tools'),
                   '--host-sysroot', str(work / 'host-inputs/host-root'), '--jobs', str(args.jobs)]
            artifacts = root / 'artifacts/nc'
        elif profile == 'a14-61':
            cmd = ['bash', str(HERE / 'build-6.1.138.sh'), str(root), str(prepared)]
            artifacts = root / 'artifacts'
        elif profile in ('a16-5', 'a16-6'):
            cmd = ['bash', str(HERE / 'build-6.12.sh'), str(root), str(prepared), profile, '4k']
            artifacts = root / 'artifacts/4k'
        else:
            cmd = ['bash', str(HERE / 'build-gki.sh'), str(root), str(prepared), profile]
            artifacts = root / 'artifacts/4k'
        started = time.time()
        print('BUILD_START', profile, record['commit'], flush=True)
        with (result / 'build.log').open('wb') as log:
            code = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        state = dict(profile=profile, source_commit=record['commit'], read_mode='nc',
                     exit_code=code, seconds=round(time.time() - started, 1))
        if not code:
            for name, expected in record['files_sha256'].items():
                if hashlib.sha256((prepared / name).read_bytes()).hexdigest() != expected:
                    raise SystemExit('Build changed committed source: ' + name)
            build = json.loads((artifacts / 'build.json').read_text())
            if build['read_mode'] != 'nc' or hashlib.sha256((artifacts / build['module']).read_bytes()).hexdigest() != build['sha256']:
                raise SystemExit('Artifact mode/hash differs: ' + profile)
            for path in artifacts.iterdir():
                if path.is_file():
                    shutil.copy2(path, result / path.name)
            state.update(module=build['module'], sha256=build['sha256'])
        results[profile] = state
        (result / 'status.json').write_text(json.dumps(state, indent=2) + '\n')
        (dest / 'matrix.json').write_text(json.dumps(results, indent=2) + '\n')
        print('BUILD_FINISH', profile, code, flush=True)
    raise SystemExit(int(any(value['exit_code'] for value in results.values())))


if __name__ == '__main__':
    main()
