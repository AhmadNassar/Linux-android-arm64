#!/usr/bin/env python3
"""Compare template instruction bytes across the 5.10 Clang 12/14 profiles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('clang12', type=Path)
parser.add_argument('clang14', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
instructions = '''fadd h0,h1,h2
fmlal v0.2s,v1.2h,v2.2h
crc32cx w0,w1,x2
casal x0,x1,[x2]
pacga x0,x1,x2
bfdot v0.4s,v1.8h,v2.8h
usdot v0.4s,v1.16b,v2.16b
smmla v0.4s,v1.16b,v2.16b
bfmmla v0.4s,v1.8h,v2.8h
bfmlalb v0.4s,v1.8h,v2.h[0]
'''
images = []
for clang, profile, name in [(args.clang12, 'neoverse-v1+crc', 'clang12'),
                              (args.clang14, 'cortex-a510+crc', 'clang14')]:
    obj = args.output / (name + '.o')
    binary = args.output / (name + '.bin')
    subprocess.run([str(clang / 'bin/clang'), '--target=aarch64-linux-gnu',
                    '-x', 'assembler', '-c', '-o', str(obj), '-'],
                   input='.cpu ' + profile + '\n' + instructions + '.arch armv8-a\n',
                   text=True, check=True)
    subprocess.run([str(clang / 'bin/llvm-objcopy'), '-O', 'binary',
                    '--only-section=.text', str(obj), str(binary)], check=True)
    images.append(binary.read_bytes())
if images[0] != images[1] or len(images[0]) != 40:
    raise SystemExit('hardware template profile changed instruction bytes')
result = {'instructions': len(images[0]) // 4, 'same_machine_code': True,
          'sha256': hashlib.sha256(images[0]).hexdigest()}
(args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
