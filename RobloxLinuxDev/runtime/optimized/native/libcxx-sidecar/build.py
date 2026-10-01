#!/usr/bin/env python3
"""Build unchanged Darling libc++ at O0/O2; all writes are sidecar-local."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess

HERE = Path(__file__).resolve().parent
A = HERE.parents[2]
SRC = A / 'src/darling'
CXX = SRC / 'src/external/libcxx'
SDK = SRC / 'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
OUT = HERE / 'build'
# The Debian runtime this build was pinned to; the -O2 Darling install keeps
# those originals under optimized/darling/stock.
BASE = A / 'optimized/darling/stock/darling-root'
BASE = BASE if BASE.exists() else A / 'darling-root'
for folder in ('logs', 'tmp'):
    (OUT / folder).mkdir(parents=True, exist_ok=True)
ENV = dict(os.environ, TMPDIR=str(OUT / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
LINKER = OUT / 'linker/ld64/x86_64-apple-darwin-ld'
FLAGS = ['clang++', '--target=x86_64-apple-macos11', '-std=c++11', '-fblocks',
         '-fexceptions', '-DDARLING', '-DDARWIN', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE',
         '-DTARGET_OS_MAC=1', '-DPLATFORM_MacOSX', '-nostdinc++',
         '-isysroot', str(A / 'darling-root'), '-nostdlibinc',
         '-isystem', str(CXX / 'include'), '-isystem', str(SDK / 'usr/include'),
         '-I', str(SRC / 'src/external/libcxxabi/include'),
         '-Wno-deprecated-declarations', '-Wno-nullability-completeness']


def run(cmd, label):
    cmd = list(map(str, cmd))
    (OUT / 'logs' / (label + '.command.json')).write_text(json.dumps(cmd, indent=2) + '\n')
    result = subprocess.run(cmd, env=ENV, capture_output=True, text=True, timeout=120)
    (OUT / 'logs' / (label + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError('Failed ' + label + '; see build/logs/' + label + '.log')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    run(['cmake', '-S', HERE / 'linker', '-B', OUT / 'linker', '-G', 'Ninja',
         '-DDARLING_SOURCE=' + str(SRC), '-DCMAKE_C_COMPILER=clang',
         '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_BUILD_TYPE=Release'], 'linker-config')
    run(['cmake', '--build', OUT / 'linker', '-j2'], 'linker-build')
    cmake = (CXX / 'CMakeLists.txt').read_text()
    sources = [CXX / name for name in re.search(r'set\(cxx_sources\s+(.*?)\n\)', cmake, re.S)[1].split()]
    inputs = sources + [CXX / 'CMakeLists.txt', LINKER]
    inputs += sorted(p for folder in ('include', 'lib', 'src/include') for p in (CXX / folder).rglob('*') if p.is_file())
    inputs += [BASE / ('usr/lib/' + n) for n in ('libc++.1.dylib', 'libc++abi.dylib', 'libSystem.B.dylib')]
    before = {str(p): digest(p) for p in inputs}
    (OUT / 'inputs-before.json').write_text(json.dumps(before, indent=2) + '\n')
    run(['clang++', '--version'], 'compiler-version')
    for mode in ('O0', 'O2'):
        dest = OUT / mode
        dest.mkdir(exist_ok=True)
        objects = []
        for p in sources:
            obj = dest / (p.name + '.o')
            run(FLAGS + ['-' + mode, '-g', '-fvisibility=hidden', '-fvisibility-inlines-hidden',
                '-D_LIBCPP_BUILDING_LIBRARY=1', '-DLIBCXX_BUILDING_LIBCXXABI=1',
                '-c', p, '-o', obj], mode + '-' + p.name)
            objects.append(obj)
        run(['clang++', '--target=x86_64-apple-macos11', '--ld-path=' + str(LINKER),
            '-dynamiclib', '-nostdlib', '-isysroot', A / 'darling-root',
            '-Wl,-install_name,/usr/lib/libc++.1.dylib,-compatibility_version,1.0.0,-current_version,1.0.0',
            '-Wl,-reexport_library,' + str(BASE / 'usr/lib/libc++abi.dylib'),
            '-Wl,-unexported_symbols_list,' + str(CXX / 'lib/libc++unexp.exp'),
            '-Wl,-force_symbols_not_weak_list,' + str(CXX / 'lib/notweak.exp'),
            '-Wl,-force_symbols_weak_list,' + str(CXX / 'lib/weak.exp'),
            *objects, '-lSystem', '-lc++abi', '-o', dest / 'libc++.1.dylib'], mode + '-link')
        print('Built', mode, flush=True)
    assert before == {str(p): digest(p) for p in inputs}, 'Shared input changed during build'
    (OUT / 'provenance.json').write_text(json.dumps(dict(source_count=len(sources), inputs=before,
        artifacts={str(p): digest(p) for p in OUT.glob('O*/libc++.1.dylib')}), indent=2) + '\n')


if __name__ == '__main__':
    main()
