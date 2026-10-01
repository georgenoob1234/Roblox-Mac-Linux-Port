#!/usr/bin/env python3
"""Check native candidates' export flags, dependency kinds, and ABI versions."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import symbolize

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rtti-only', action='store_true', help='check only O0/rtti-O2 libc++abi candidates')
args = parser.parse_args()


def inspect(path):
    base, _, _, digest = symbolize.macho(path)
    data = path.read_bytes()[base:]
    libraries, pos = {}, 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, pos)
        if cmd in (0xc, 0xd, 0x80000018, 0x8000001f, 0x80000023):
            start = pos + struct.unpack_from('<I', data, pos + 8)[0]
            name = data[start:data.index(0, start)].decode()
            current, compat = struct.unpack_from('<II', data, pos + 16)
            libraries[name] = dict(kind=cmd, current=current, compatible=compat)
        pos += size
    text = subprocess.check_output(['llvm-objdump', '--macho', '--arch=x86_64', '--exports-trie', str(path)], text=True)
    exports = sorted(re.findall(r'^0x[0-9a-fA-F]+\s+(.+)$', text, re.M))
    assert exports
    return dict(sha256=digest, libraries=libraries, exports=exports)


reports = []
candidates = [('libc++abi.dylib', ('O0', 'rtti-O2'))]
if not args.rtti_only:
    candidates.append(('system/libsystem_asl.dylib', ('asl-O0', 'asl-O2')))
for name, modes in candidates:
    original = symbolize.ROOT / 'runtime/darling-root/usr/lib' / name
    stock = symbolize.ROOT / 'runtime/optimized/darling/stock/darling-root/usr/lib' / name
    if stock.is_file():
        original = stock  # Validate against the shipped ABI even after an O2 install.
    before = inspect(original)
    for mode in modes:
        candidate = here / 'build' / mode / Path(name).name
        after = inspect(candidate)
        assert before['exports'] == after['exports'], (mode, 'exports/flags')
        assert before['libraries'].keys() == after['libraries'].keys(), (mode, 'dependency set')
        differences = {}
        for lib, old in before['libraries'].items():
            new = after['libraries'][lib]
            assert old['kind'] == new['kind'] and old['compatible'] == new['compatible'], (mode, lib)
            if old['current'] != new['current']:
                differences[lib] = {'baseline': old['current'], 'candidate': new['current']}
        # Packaged libdyld's own ID is 421.1, while old ASL records 0.
        assert differences in ({}, {'/usr/lib/system/libdyld.dylib': {'baseline': 0, 'candidate': 27590912}}), differences
        reports.append(dict(candidate=str(candidate), baseline=before, after=after, current_version_differences=differences))
        print('PASS', mode, len(after['exports']), 'matching exports/flags, dependency kinds and compatibility')
(here / ('build/abi-rtti.json' if args.rtti_only else 'build/abi.json')).write_text(json.dumps(reports, indent=2) + '\n')
