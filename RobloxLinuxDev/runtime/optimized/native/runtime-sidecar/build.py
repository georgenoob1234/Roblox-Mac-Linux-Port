#!/usr/bin/env python3
"""Isolated native RTTI candidate: original sources, only private_typeinfo at O2."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
A = HERE.parents[2]
SRC = A / 'src/darling'
ABI = SRC / 'src/external/libcxxabi'
SDK = SRC / 'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
OUT = HERE / 'build'
OUT.mkdir(exist_ok=True)
(OUT / 'tmp').mkdir(exist_ok=True)
ENV = dict(os.environ, TMPDIR=str(OUT / 'tmp'))
COMMANDS = []
FLAGS = ['clang++', '--target=x86_64-apple-macos11', '-std=c++11', '-fblocks',
         '-DDARLING', '-D__DARWIN_NON_CANCELABLE=1', '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1',
         '-D_LIBCPP_HAS_NO_LIBRARY_ALIGNED_ALLOCATION=1',
         '-isysroot', str(A / 'darling-root'), '-nostdinc++',
         '-isystem', str(SRC / 'src/external/libcxx/include'),
         '-isystem', str(SDK / 'usr/include'), '-I', str(ABI / 'include'),
         '-I', str(SRC / 'src/external/libcxx')]


def run(command, label):
    command = list(map(str, command))
    COMMANDS.append(command)
    result = subprocess.run(command, env=ENV, capture_output=True, text=True, timeout=90)
    (OUT / (label + '.log')).write_text(result.stdout + result.stderr)
    (OUT / (Path(sys.argv[0]).stem + '-commands.json')).write_text(json.dumps(COMMANDS, indent=2) + '\n')
    if result.returncode:
        raise SystemExit('FAIL ' + label + '; see build/' + label + '.log')


def main():
    names = re.search(r'set\(cxxabi_sources\s+(.*?)\n\)', (ABI / 'CMakeLists.txt').read_text(), re.S)[1]
    sources = [ABI / line.strip() for line in names.splitlines() if line.strip() and not line.strip().startswith('#')]
    objects = []
    for source in sources:
        obj = OUT / (source.stem + '.o')
        run(FLAGS + ['-O0', '-c', source, '-o', obj], source.stem)
        objects.append(obj)
    for mode in ('O0', 'rtti-O2'):
        dest = OUT / mode
        dest.mkdir(exist_ok=True)
        selected = list(objects)
        if mode == 'rtti-O2':
            obj = dest / 'private_typeinfo.o'
            run(FLAGS + ['-O2', '-c', ABI / 'src/private_typeinfo.cpp', '-o', obj], mode)
            selected[selected.index(OUT / 'private_typeinfo.o')] = obj
        run(['clang++', '--target=x86_64-apple-macos11', '-fuse-ld=lld', '-nostdlib', '-dynamiclib',
             '-isysroot', A / 'darling-root', '-Wl,--threads=2,-no_adhoc_codesign',
             '-Wl,-install_name,/usr/lib/libc++abi.dylib,-compatibility_version,1.0.0,-current_version,1.0.0',
             *selected, '-lSystem', '-o', dest / 'libc++abi.dylib'], mode + '-link')
        shutil.copy2(A / 'shims/metal/sort-weak-bindings.py', dest / 'sort-weak-bindings.py')
        run(['python3', '-B', dest / 'sort-weak-bindings.py', dest / 'libc++abi.dylib'], mode + '-weak')
    receipt = {'sources': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
               'libraries': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in OUT.glob('*/libc++abi.dylib')}}
    (OUT / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('PASS isolated RTTI O0/rtti-O2 build')


if __name__ == '__main__':
    main()
