#!/usr/bin/env python3
"""Check package provenance; optionally smoke-test an AppDir on a baseline x86-64 CPU."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from portable_runtime import checked_records

record = 'core|libstdc++|x86_64|16.2.1-1|' + 'a' * 64 + '|https://mirror/example.pkg.tar.zst'
assert checked_records(record)[0]['arch'] == 'x86_64'
assert checked_records(record)[0]['url'].startswith('https://archive.archlinux.org/packages/')
for invalid in ('', record.replace('x86_64', 'x86_64_v4'), record.replace('x86_64', 'x86_64_v3'),
                record.replace('core|', 'cachyos|'), record.replace('a' * 64, 'missing')):
    try:
        checked_records(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError(f'Accepted nonportable/unverified package: {invalid}')
print('PASS package provenance: standard repositories, baseline architecture, SHA-256 required')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--appdir', type=Path)
parser.add_argument('--sysroot', type=Path)
parser.add_argument('--qemu', type=Path)
parser.add_argument('--llvm-as', type=Path, help='Assembler outside the test sysroot, if needed')
args = parser.parse_args()
if args.appdir:
    assert not list((args.appdir / 'browser/lib').glob('libwayland-*.so*')), 'Use host Wayland with host EGL drivers'
    assert not list((args.appdir / 'browser/lib').glob('libxcb*.so*')), 'Use host XCB with host Vulkan drivers'
    count = 0
    failures = []
    for path in args.appdir.rglob('*'):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open('rb') as file:
            if file.read(4) != b'\x7fELF':
                continue
        note_result = subprocess.run(['readelf', '-n', str(path)], text=True, capture_output=True)
        # Ubuntu's binutils predates SDL's FDO dlopen metadata and returns 1
        # for that unknown note, while still decoding the GNU ISA requirements.
        if note_result.returncode and (note_result.stderr or 'Unknown note type' not in note_result.stdout):
            note_result.check_returncode()
        notes = note_result.stdout
        assert not re.search(r'x86 ISA needed:.*x86-64-v[234]', notes), f'CPU-specific executable/library: {path}'
        symbols = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(path)], text=True)
        versions = [tuple(map(int, version.split('.'))) for version in
                    re.findall(r' UND .*?@GLIBC_([0-9.]+)', symbols)]
        if versions and max(versions) > (2, 39):
            failures.append(f'{path.relative_to(args.appdir)}: GLIBC_{".".join(map(str, max(versions)))}')
        count += 1
    if failures:
        raise SystemExit('Release requires newer than the glibc 2.39 baseline:\n' + '\n'.join(failures))
    print(f'PASS baseline ELF requirements: {count} files')
if args.sysroot or args.qemu:
    if not all((args.appdir, args.sysroot, args.qemu)):
        parser.error('--appdir, --sysroot and --qemu must be supplied together')
    app, prefix, qemu = (p.resolve() for p in (args.appdir, args.sysroot, args.qemu))
    def run(binary, *arguments, expected=0):
        result = subprocess.run([str(qemu), '-cpu', 'qemu64', '-L', str(prefix),
            '-E', f'LD_LIBRARY_PATH={app}/browser/lib', str(binary), *map(str, arguments)],
            capture_output=True, text=True, timeout=30)
        assert result.returncode == expected, (binary, result.returncode, result.stderr)
        return result.stdout
    assert 'LLVM version' in run(app / 'usr/bin/llvm-dis', '--version')
    assert 'SPIRV-Tools' in run(app / 'usr/bin/spirv-val', '--version')
    run(app / 'usr/bin/metal2vulkan', '--help')
    # This invocation exits at the browser's argument check, after shared-library
    # constructors have run. It cannot start Roblox or open a window.
    run(app / 'browser/roblox-gtk.bin', expected=2)
    for binary in ('darlingserver', 'darling-cli', 'darling-root/usr/libexec/darling/mldr'):
        run(app / binary, expected=1)
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / 'smoke.ll'
        bitcode = source.with_suffix('.bc')
        output = source.with_suffix('.out.ll')
        source.write_text('define void @smoke() { ret void }\n')
        run(args.llvm_as or prefix / 'usr/bin/llvm-as', source, '-o', bitcode)
        run(app / 'usr/bin/llvm-dis', bitcode, '-o', output)
        assert 'define void @smoke()' in output.read_text()
        probe = Path(tmp) / 'load-ui'
        source = Path(tmp) / 'load-ui.c'
        source.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                          'int main(int argc,char **argv){if(dlopen(argv[1],RTLD_NOW))return 0;'
                          'fprintf(stderr,"%s\\n",dlerror());return 1;}\n')
        subprocess.run(['cc', '-B' + str(prefix / 'usr/lib') + '/', '-march=x86-64', '-mtune=generic',
                        str(source), '-ldl', '-o', str(probe)], check=True)
        run(probe, app / 'optimized/ui/libroblox-wayland.so')
    print('PASS baseline CPU without AVX: LLVM disassembly, SPIR-V tool, translator, native UI loading and Darling startup')
