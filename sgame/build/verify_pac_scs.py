#!/usr/bin/env python3
"""Check AArch64 module instructions and the dynamic-SCS CFI patch locations.

Uses only Python's standard library. Run on the final .ko, after LTO/linking.
The CFI walk covers the opcodes accepted by the pinned GKI 6.1 patch-scs.c.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

PACIASP, AUTIASP = 0xD503233F, 0xD50323BF
RETAA, RETAB = 0xD65F0BFF, 0xD65F0FFF


def uleb(data, offset):
    value, shift = 0, 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
        shift += 7


def cfi_locations(data, offset, end, address, alignment):
    sites = []
    while offset < end:
        op = data[offset]
        offset += 1
        if op in (0, 0x0A, 0x0B) or op >= 0xC0:
            continue
        if op in (2, 3):
            width = 1 if op == 2 else 2
            address += int.from_bytes(data[offset:offset + width], 'little') * alignment
            offset += width
        elif op in (0x05, 0x0C):
            _, offset = uleb(data, offset)
            _, offset = uleb(data, offset)
        elif op in (0x06, 0x08, 0x0D, 0x0E, 0x13) or 0x80 <= op <= 0xBF:
            _, offset = uleb(data, offset)
        elif op == 0x2D:
            sites.append(address - 4)
        elif 0x40 <= op <= 0x7F:
            address += (op & 63) * alignment
        else:
            raise ValueError(f'CFI opcode unsupported by pinned GKI: 0x{op:02x}')
    return sites


def verify(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected ELF64 little-endian AArch64 module')
    section_offset = struct.unpack_from('<Q', data, 40)[0]
    entry_size, count, names_index = struct.unpack_from('<HHH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_offset + i * entry_size)
                for i in range(count)]

    def contents(index):
        sec = sections[index]
        return data[sec[4]:sec[4] + sec[5]]

    def string(blob, offset):
        return blob[offset:blob.index(b'\0', offset)].decode()

    names = contents(names_index)
    indices = {string(names, sec[0]): i for i, sec in enumerate(sections)}
    symbol_section = sections[indices['.symtab']]
    symbol_names = contents(symbol_section[6])
    symbols = [struct.unpack_from('<IBBHQQ', data, offset)
               for offset in range(symbol_section[4], symbol_section[4] + symbol_section[5], 24)]
    init = next(sym for sym in symbols if string(symbol_names, sym[0]) == 'init_module')
    init_section, init_start, init_size = init[3:6]
    code = {(i, offset): struct.unpack_from('<I', contents(i), offset)[0]
            for i, sec in enumerate(sections) if sec[1] == 1 and sec[2] & 4
            for offset in range(0, sec[5] - 3, 4)}
    authenticated = {site for site, word in code.items() if word in (RETAA, RETAB)}
    hints = {site for site, word in code.items() if word in (PACIASP, AUTIASP)}
    frame_index = indices['.init.eh_frame']
    frame = contents(frame_index)
    relocations = {}
    for i, sec in enumerate(sections):
        if sec[1] != 4 or sec[7] != frame_index:
            continue
        raw = contents(i)
        for offset in range(0, len(raw), 24):
            address, info, addend = struct.unpack_from('<QQq', raw, offset)
            sym = symbols[info >> 32]
            relocations[address] = (sym[3], sym[4] + addend)
    patched = set()
    cie, frames, init_frames = {}, 0, 0
    offset = 0
    while offset + 4 < len(frame):
        size = struct.unpack_from('<I', frame, offset)[0]
        if not size:
            break
        if size == 0xFFFFFFFF or offset + size + 4 > len(frame):
            raise ValueError('unsupported/truncated CFI frame')
        end = offset + size + 4
        pointer = struct.unpack_from('<I', frame, offset + 4)[0]
        if not pointer:
            aug_end = frame.index(b'\0', offset + 9)
            augmentation = frame[offset + 9:aug_end]
            alignment, _ = uleb(frame, aug_end + 1)
            cie[offset] = (augmentation.startswith(b'z'), alignment)
        else:
            augmented, alignment = cie[offset + 4 - pointer]
            section, address = relocations[offset + 8]
            cursor = offset + 16
            if augmented:
                length, cursor = uleb(frame, cursor)
                cursor += length
            patched.update((section, site) for site in
                           cfi_locations(frame, cursor, end, address, alignment)
                           if code.get((section, site)) in (PACIASP, AUTIASP))
            frames += 1
            if section == init_section and address == init_start:
                init_frames += 1
        offset = end
    init_words = [code[(init_section, offset)] for offset in range(init_start, init_start + init_size, 4)]
    missing = hints - patched
    problems = []
    if authenticated:
        problems.append(f'{len(authenticated)} RETAA/RETAB instructions remain')
    if missing:
        problems.append(f'{len(missing)} PACIASP/AUTIASP sites lack dynamic-SCS CFI locations')
    if not init_frames or PACIASP not in init_words or AUTIASP not in init_words:
        problems.append('init_module lacks its CFI frame or PACIASP/AUTIASP return pair')
    return {
        'status': 'FAIL' if problems else 'PASS', 'module': path.name,
        'sha256': hashlib.sha256(data).hexdigest(),
        'retaa_retab_count': len(authenticated), 'pac_hint_sites': len(hints),
        'scs_patchable_sites': len(patched), 'missing_cfi_sites': len(missing),
        'fde_count': frames, 'init_module_fde_count': init_frames,
        'init_module_bytes': init_size,
        'init_module_paciasp': init_words.count(PACIASP),
        'init_module_autiasp': init_words.count(AUTIASP),
        'problems': problems,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    record = verify(args.module)
    result = json.dumps(record, indent=2) + '\n'
    if args.output:
        args.output.write_text(result)
    print(result, end='')
    return bool(record['problems'])


if __name__ == '__main__':
    raise SystemExit(main())
