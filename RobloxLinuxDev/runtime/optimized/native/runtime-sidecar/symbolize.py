#!/usr/bin/env python3
"""Resolve saved native IPs only; never inspect or sample a live process."""
from pathlib import Path
import bisect
import collections
import hashlib
import json
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def local_image(name):
    local = Path(name.removesuffix(' (deleted)'))
    if local.is_file():
        return local
    for prefix in ('/usr/lib/', '/System/Library/'):
        if prefix in name:
            return ROOT / ('runtime/darling-root' + prefix) / name.split(prefix, 1)[1]
    # Basename fallback for native shims from an unmounted AppImage. Hashes in
    # the report identify the substitute; this cannot prove historical identity.
    filename = local.name
    candidates = [ROOT / folder / filename for folder in (
        'runtime/darling-root/usr/lib/system', 'runtime/darling-root/usr/lib',
        'runtime/optimized/native/lib', 'RobloxPlayer.app/Contents/MacOS')]
    return next((p for p in candidates if p.is_file()), local)


def vm_address(ip, start, mapping_offset, slice_offset, segments):
    offset = ip - start + mapping_offset - slice_offset
    for vm, fileoff, size in segments:
        if fileoff <= offset < fileoff + size:
            return vm + offset - fileoff
    raise ValueError('sample is outside file-backed Mach-O segments')


def macho(path):
    data = path.read_bytes()
    base = 0
    if data[:4] == b'\xca\xfe\xba\xbe':
        slices = [struct.unpack_from('>IIIII', data, 8 + i * 20)
                  for i in range(struct.unpack_from('>I', data, 4)[0])]
        base = next(offset for cpu, _, offset, _, _ in slices if cpu == 0x1000007)
    if data[base:base + 8] != b'\xcf\xfa\xed\xfe\x07\x00\x00\x01':
        raise ValueError('not native x86_64 Mach-O')
    pos = base + 32
    segments, sections = [], []
    symtab = None
    for _ in range(struct.unpack_from('<I', data, base + 16)[0]):
        cmd, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > len(data):
            raise ValueError('invalid load command')
        if cmd == 0x19:
            _, vm, _, offset, length = struct.unpack_from('<16sQQQQ', data, pos + 8)
            segments.append((vm, offset, length))
            for i in range(struct.unpack_from('<I', data, pos + 64)[0]):
                sec = struct.unpack_from('<16s16sQQIIIIIIII', data, pos + 72 + i * 80)
                sections.append((sec[2], sec[3], sec[8]))
        elif cmd == 2:
            symtab = struct.unpack_from('<IIII', data, pos + 8)
        pos += size
    symbols = []
    if symtab:
        symoff, count, stroff, strsize = symtab
        strings = data[base + stroff:base + stroff + strsize]
        for i in range(count):
            name, kind, section, _, address = struct.unpack_from('<IBBHQ', data, base + symoff + i * 16)
            if kind & 0xe0 or kind & 0x0e != 0x0e or not 0 < section <= len(sections):
                continue
            va, length, flags = sections[section - 1]
            if flags & 0x80000400 and va <= address < va + length:
                end = strings.find(b'\0', name)
                if end < 0:
                    raise ValueError('invalid string table')
                symbols.append((address, strings[name:end].decode(), va + length))
    return base, segments, sorted(symbols), hashlib.sha256(data).hexdigest()


def main():
    # Nonzero fat slice, nonidentity file/VM offsets, and an executable's 4 GB base.
    assert vm_address(0x7f9b9688474b, 0x7f9b9679a000, 0x276000, 0x276000,
                      [(0, 0, 0x141000)]) == 0xea74b
    assert vm_address(0x900123, 0x900000, 0x7000, 0x1000,
                      [(0x20000, 0x6000, 0x1000)]) == 0x20123
    assert vm_address(0x100001234, 0x100000000, 0, 0,
                      [(0x100000000, 0, 0x2000)]) == 0x100001234
    profile = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'runtime/logs/port-20260908/native-cpu.json'
    saved = json.loads(profile.read_text())
    maps = []
    for line in saved['maps'].splitlines():
        region, perms, offset, _, inode, *path = line.split(maxsplit=5)
        start, end = (int(x, 16) for x in region.split('-'))
        maps.append((start, end, int(offset, 16), path[0] if path else '[anonymous]', int(inode)))
    maps.sort()
    starts = [m[0] for m in maps]
    by_image = collections.defaultdict(list)
    totals = {'main': 0, 'all': 0}
    for tid, ip, count in saved['samples']:
        ip = int(ip, 16)
        index = bisect.bisect_right(starts, ip) - 1
        if index < 0 or ip >= maps[index][1]:
            raise ValueError('unmapped sample')
        mapping = maps[index]
        by_image[mapping[3]].append((tid, ip, count, mapping))
        totals['all'] += count
        if tid == saved['pid']:
            totals['main'] += count
    result = {'profile': str(profile), 'sha256': hashlib.sha256(profile.read_bytes()).hexdigest(),
              'totals': totals, 'exact_ip_samples': saved['exact_ip_samples'], 'images': []}
    for name, samples in by_image.items():
        if not any(x in name for x in ('dyld', 'libc++', 'pthread', 'kqueue', 'malloc', 'libsystem_')):
            continue
        local = local_image(name)
        entry = {'mapped_path': name, 'local_path': str(local)}
        try:
            base, segments, symbols, digest = macho(local)
        except (OSError, ValueError):
            continue
        entry.update(sha256=digest, slice_offset=hex(base), mapped_inode=samples[0][3][4],
                     local_inode=local.stat().st_ino)
        keys = [s[0] for s in symbols]
        for scope in totals:
            counts = collections.Counter()
            ips = collections.defaultdict(collections.Counter)
            for tid, ip, count, (start, _, offset, _, _) in samples:
                if scope == 'main' and tid != saved['pid']:
                    continue
                vm = vm_address(ip, start, offset, base, segments)
                index = bisect.bisect_right(keys, vm) - 1
                symbol = symbols[index][1] if index >= 0 and vm < symbols[index][2] else '[no text symbol]'
                counts[symbol] += count
                ips[symbol][(ip, vm, keys[index] if index >= 0 else 0)] += count
            entry[scope] = {'samples': sum(counts.values()),
                            'percent': 100 * sum(counts.values()) / totals[scope], 'symbols': []}
            for symbol, count in counts.most_common():
                (ip, vm, address), _ = ips[symbol].most_common(1)[0]
                entry[scope]['symbols'].append(dict(symbol=symbol, samples=count, ip=hex(ip),
                                                   vm=hex(vm), symbol_offset=hex(vm - address)))
        result['images'].append(entry)
    if saved.get('chains'):
        cache = {}

        def resolve(raw, return_address=False):
            # Callchain parents contain return PCs; attribute their call sites.
            ip = int(raw, 16) - int(return_address)
            index = bisect.bisect_right(starts, ip) - 1
            if index < 0 or ip >= maps[index][1]:
                return '[unmapped]'
            start, _, offset, name, _ = maps[index]
            if name not in cache:
                try:
                    base, segments, symbols, _ = macho(local_image(name))
                    cache[name] = (base, segments, symbols, [s[0] for s in symbols])
                except (OSError, ValueError, StopIteration):
                    cache[name] = None
            if cache[name] is None:
                return Path(name).name + '+file:' + hex(ip - start + offset)
            base, segments, symbols, keys = cache[name]
            try:
                vm = vm_address(ip, start, offset, base, segments)
            except ValueError:
                return Path(name).name + ':[not file-backed]'
            index = bisect.bisect_right(keys, vm) - 1
            symbol = symbols[index][1] if index >= 0 and vm < symbols[index][2] else '[no text symbol]'
            return Path(name).name + ':' + symbol

        chains = collections.Counter()
        for tid, chain, count in saved['chains']:
            if tid == saved['pid'] and chain and resolve(chain[0]) == 'dyld:_linux_syscall':
                chains[tuple(resolve(ip, i > 0) for i, ip in enumerate(chain[:20]))] += count
        result['dyld_syscall_chains'] = [dict(samples=n, chain=list(c)) for c, n in chains.most_common()]
    (HERE / 'build').mkdir(exist_ok=True)
    output = HERE / ('build/' + profile.stem + '-symbols.json')
    output.write_text(json.dumps(result, indent=2) + '\n')
    for item in sorted(result['images'], key=lambda x: -x['main']['samples']):
        print(Path(item['mapped_path']).name, item['main']['samples'],
              [(s['symbol'], s['samples']) for s in item['main']['symbols'][:3]])
    print('PASS saved-profile VM checks;', output)


if __name__ == '__main__':
    main()
