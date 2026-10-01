#!/usr/bin/env python3
"""Runtime-sidecar ABI inspection, extended to exact versions and reexports."""
import importlib.util
import json
import re
import struct
import subprocess
import build

spec = importlib.util.spec_from_file_location('symbolize', build.HERE.parent / 'runtime-sidecar/symbolize.py')
symbolize = importlib.util.module_from_spec(spec)
spec.loader.exec_module(symbolize)


def inspect(path, label):
    base, _, _, digest = symbolize.macho(path)
    data = path.read_bytes()[base:]
    libraries, pos = [], 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, pos)
        if cmd in (0xc, 0xd, 0x80000018, 0x8000001f, 0x80000023):
            start = pos + struct.unpack_from('<I', data, pos + 8)[0]
            name = data[start:data.index(0, start)].decode()
            current, compat = struct.unpack_from('<II', data, pos + 16)
            libraries.append(dict(name=name, kind=cmd, current=current, compatible=compat))
        pos += size
    trie = subprocess.check_output(['llvm-objdump', '--macho', '--arch=x86_64', '--exports-trie', str(path)], text=True)
    (build.OUT / 'logs' / (label + '-exports.txt')).write_text(trie)
    exports = sorted(re.sub(r'^0x[0-9a-fA-F]+\s+', '', line) for line in trie.splitlines()
                     if re.match(r'^(0x[0-9a-fA-F]+\s|\[re-export\])', line))
    assert exports
    return dict(sha256=digest, libraries=libraries, exports=exports)


if __name__ == '__main__':
    paths = {'installed': build.BASE / 'usr/lib/libc++.1.dylib',
             **{m: build.OUT / m / 'libc++.1.dylib' for m in ('O0', 'O2')}}
    reports = {m: inspect(p, m) for m, p in paths.items()}
    (build.OUT / 'abi.json').write_text(json.dumps(reports, indent=2) + '\n')
    for mode in ('O0', 'O2'):
        old, new = reports['installed'], reports[mode]
        assert old['exports'] == new['exports'], (mode, 'export names/flags/reexport routes',
            sorted(set(old['exports']) - set(new['exports']))[:15],
            sorted(set(new['exports']) - set(old['exports']))[:15])
        assert old['libraries'] == new['libraries'], (mode, 'dependency order/kind/versions')
        print('PASS', mode, len(new['exports']), 'exact exports/flags/routes and ordered dependencies/ID/versions')
    receipt = json.loads((build.OUT / 'provenance.json').read_text())
    for name, expected in {**receipt['inputs'], **receipt['artifacts']}.items():
        assert build.digest(build.Path(name)) == expected, name + ' changed'
    print('PASS input and artifact hashes')
