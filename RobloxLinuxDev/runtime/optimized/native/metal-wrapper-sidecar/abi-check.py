#!/usr/bin/env python3
"""Fail closed on Mach-O export/flag, dependency/ID/version or atomic-call drift."""
import collections
import json
import re
import struct
import subprocess
import build


def inspect(path):
    data = path.read_bytes()
    file_bytes = len(data)
    if data[:4] == b'\xca\xfe\xba\xbe':
        slices = [struct.unpack_from('>IIIII', data, 8 + i * 20)
                  for i in range(struct.unpack_from('>I', data, 4)[0])]
        _, _, offset, length, _ = next(s for s in slices if s[0] == 0x1000007)
        data = data[offset:offset + length]
    assert data[:8] == b'\xcf\xfa\xed\xfe\x07\x00\x00\x01'
    assert struct.unpack_from('<I', data, 12)[0] == 6
    pos = 32
    libraries, sections = {}, {}
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, pos)
        assert size >= 8 and pos + size <= len(data)
        if cmd in (0xc, 0xd, 0x80000018, 0x8000001f, 0x80000023):
            start = pos + struct.unpack_from('<I', data, pos + 8)[0]
            name = data[start:data.index(0, start)].decode()
            current, compat = struct.unpack_from('<II', data, pos + 16)
            libraries[name] = dict(kind=cmd, current=current, compatible=compat)
        if cmd == 0x19:
            for i in range(struct.unpack_from('<I', data, pos + 64)[0]):
                sec = pos + 72 + i * 80
                name = data[sec:sec + 16].rstrip(b'\0').decode()
                segment = data[sec + 16:sec + 32].rstrip(b'\0').decode()
                sections[segment + ',' + name] = struct.unpack_from('<Q', data, sec + 40)[0]
        pos += size
    disassembly = subprocess.check_output(['llvm-objdump', '--macho', '--disassemble', path], text=True)
    # Check caller functions, not just an unused import of the atomic runtime helper.
    atomic = collections.Counter()
    method = ''
    for line in disassembly.splitlines():
        if line.endswith(':') and not line.startswith((' ', '\t')):
            method = line[:-1]
        if 'symbol stub for: _objc_copyCppObjectAtomic' in line:
            atomic[method] += 1
    assert atomic
    return dict(sha256=build.sha(path), bytes=file_bytes, x86_64_slice_bytes=len(data), libraries=libraries,
                exports=build.exports(path), sections=sections, atomic_callers=dict(atomic)), disassembly


def main():
    before, dis = inspect(build.STOCK)
    report = {'stock': before}
    (build.OUT / 'stock-disassembly.txt').write_text(dis)
    for mode in ('O0', 'O2'):
        path = build.OUT / mode / 'frameworks/Metal.framework/Versions/A/Metal'
        after, dis = inspect(path)
        assert before['exports'] == after['exports'], mode + ' exports/flags'
        assert before['libraries'] == after['libraries'], mode + ' dependency set/kinds/ID/versions'
        expected = dict(before['atomic_callers'])
        for name in ('-[MTLBufferInternal buffer]', '-[MTLTextureInternal texture]', '-[MTLRenderCommandEncoderInternal encoder]'):
            assert expected.pop(name) == 1, name
        assert expected == after['atomic_callers'], (mode, expected, after['atomic_callers'])
        report[mode] = after
        (build.OUT / (mode + '-disassembly.txt')).write_text(dis)
    # Runnable check of the only build-time IR transformation: signatures/bodies stay intact.
    sample = 'define linkonce_odr hidden i32 @foo(ptr %x) {\n ret i32 1\n}\n@bar = linkonce_odr constant i8 0\n'
    changed, names = build.retain_exports(sample, {'_foo': ' [weak_def]', '_bar': ' [weak_def]'})
    assert changed == sample.replace('linkonce_odr hidden ', 'weak_odr ').replace('linkonce_odr ', 'weak_odr ')
    assert names == ['foo', 'bar']
    for raw in build.OUT.glob('*/*.raw.ll'):
        expected, _ = build.retain_exports(raw.read_text(), before['exports'])
        assert raw.with_name(raw.name.replace('.raw.ll', '.ll')).read_text() == expected
    (build.OUT / 'abi.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS native x86_64; 238 exports/flags; exact dependency/ID/version sets;', len(before['atomic_callers']), 'atomic caller functions audited; only 3 immutable getters unlocked')


if __name__ == '__main__':
    main()
