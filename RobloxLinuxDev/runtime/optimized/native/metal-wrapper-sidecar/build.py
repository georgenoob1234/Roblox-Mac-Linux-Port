#!/usr/bin/env python3
"""Build original Metal wrapper only, with all outputs confined to this directory."""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
A = HERE.parents[2]
SRC = A / 'src/darling'
METAL = SRC / 'src/external/metal'
SDK = SRC / 'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
OUT = HERE / 'build'
ROOT = A / 'darling-root'
STOCK = ROOT / 'System/Library/Frameworks/Metal.framework/Versions/A/Metal'
FLAGS = ['clang++', '--target=x86_64-apple-macos11', '-std=c++17', '-fblocks',
         '-fno-objc-arc', '-fexceptions', '-fobjc-exceptions', '-DDARLING',
         '-DPLATFORM_MacOSX', '-DDARLING_METAL_ENABLED=1', '-DTARGET_OS_WASI=0',
         '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1', '-isysroot', str(ROOT), '-nostdinc++',
         '-isystem', str(SRC / 'src/external/libcxx/include'),
         '-isystem', str(SDK / 'usr/include'),
         '-I', str(METAL / 'include'), '-I', str(METAL / 'private-include'),
         '-I', str(METAL / 'deps/indium/include'),
         '-I', str(SRC / 'src/frameworks/CoreServices/include'),
         '-F', str(SDK / 'System/Library/Frameworks'), '-Wno-nullability-completeness']
COMMANDS = []


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, label):
    (OUT / 'tmp').mkdir(parents=True, exist_ok=True)
    command = list(map(str, command))
    result = subprocess.run(command, env=dict(os.environ, TMPDIR=str(OUT / 'tmp'),
                            PYTHONDONTWRITEBYTECODE='1'), capture_output=True, text=True, timeout=180)
    COMMANDS.append(dict(command=command, status=result.returncode))
    (OUT / (Path(sys.argv[0]).stem + '-commands.json')).write_text(json.dumps(COMMANDS, indent=2) + '\n')
    (OUT / (label + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'FAIL {label}: {result.stderr[-2000:]}')
    return result.stdout


def exports(path):
    text = subprocess.check_output(['llvm-objdump', '--macho', '--exports-trie', path], text=True)
    return dict(re.findall(r'^0x[0-9A-Fa-f]+\s+(\S+)(.*)$', text, re.M))


def retain_exports(ir, baseline):
    """Keep existing weak ABI entry points through O2 without changing their bodies."""
    kept = []
    lines = ir.splitlines(True)
    for i, line in enumerate(lines):
        constant = re.match(r'@([^ ]+) = linkonce_odr (?:hidden )?constant ', line)
        if constant and '_' + constant[1] in baseline:
            assert baseline['_' + constant[1]].strip() == '[weak_def]'
            lines[i] = line.replace('linkonce_odr hidden ', 'weak_odr ', 1).replace('linkonce_odr ', 'weak_odr ', 1)
            kept.append(constant[1])
        match = re.match(r'define linkonce_odr (?:hidden )?.*?@([^ (]+)\(', line)
        if match and '_' + match[1] in baseline:
            assert baseline['_' + match[1]].strip() == '[weak_def]'
            lines[i] = line.replace('define linkonce_odr hidden ', 'define weak_odr ', 1).replace(
                'define linkonce_odr ', 'define weak_odr ', 1)
            kept.append(match[1])
    return ''.join(lines), kept


def main():
    OUT.mkdir(exist_ok=True)
    cmake = (METAL / 'CMakeLists.txt').read_text()
    sources = [METAL / s for s in re.findall(r'^\s*(src/Metal/\S+\.mm)', cmake, re.M)]
    assert len(sources) == 20
    inputs = sources + [METAL / 'CMakeLists.txt']
    for folder in (METAL / 'include', METAL / 'private-include', METAL / 'deps/indium/include',
                   SRC / 'src/external/libcxx/include'):
        inputs += [p for p in folder.rglob('*') if p.is_file()]
    inputs += [STOCK, ROOT / 'usr/lib/darling/libindium.dylib',
               A / 'optimized/renderer/libindium.dylib', A / 'bin/roblox-mac',
               A / 'shims/metal/sort-weak-bindings.py']
    before = {str(p): sha(p) for p in inputs}
    (OUT / 'inputs-before.json').write_text(json.dumps(before, indent=2) + '\n')
    baseline = exports(STOCK)
    (OUT / 'exports.txt').write_text('\n'.join(sorted(baseline)) + '\n')
    for mode in ('O0', 'O2'):
        dest = OUT / mode
        dest.mkdir(exist_ok=True)
        objects = []
        retained = {}
        for source in sources:
            obj = dest / (source.stem + '.o')
            raw = obj.with_suffix('.raw.ll')
            run(FLAGS + ['-' + mode, '-Xclang', '-disable-llvm-passes', '-MD', '-MF', obj.with_suffix('.d'),
                        '-S', '-emit-llvm', source, '-o', raw], mode + '-' + source.stem)
            ir, retained[source.name] = retain_exports(raw.read_text(), baseline)
            kept = obj.with_suffix('.ll')
            kept.write_text(ir)
            run(['clang++', '--target=x86_64-apple-macos11', '-' + mode, '-c', kept, '-o', obj],
                mode + '-' + source.stem + '-optimize')
            objects.append(obj)
        (dest / 'retained-exports.json').write_text(json.dumps(retained, indent=2) + '\n')
        run(['clang++', '--target=x86_64-apple-macos11', '-fuse-ld=lld', '-nostdlib', '-dynamiclib',
             '-isysroot', ROOT, '-Wl,--threads=2,-no_adhoc_codesign',
             '-Wl,-install_name,/System/Library/Frameworks/Metal.framework/Versions/A/Metal',
             '-Wl,-compatibility_version,1.0.0,-current_version,1.0.0', *objects,
             '-Wl,-exported_symbols_list,' + str(OUT / 'exports.txt'),
             '-lobjc', '-framework', 'Foundation', ROOT / 'usr/lib/darling/libindium.dylib',
             '-lc++', '-lSystem', '-framework', 'CoreFoundation', '-lc++abi',
             '-o', dest / 'Metal.dylib'], mode + '-link')
        shutil.copy2(A / 'shims/metal/sort-weak-bindings.py', dest / 'sort-weak-bindings.py')
        run(['python3', '-B', dest / 'sort-weak-bindings.py', dest / 'Metal.dylib'], mode + '-weak')
        shutil.copy2(dest / 'Metal.dylib', dest / 'Metal')
        after = exports(dest / 'Metal')
        delta = dict(missing=sorted(baseline.keys() - after.keys()), added=sorted(after.keys() - baseline.keys()),
                     flags={k: [baseline[k], after[k]] for k in baseline.keys() & after.keys() if baseline[k] != after[k]})
        (dest / 'exports-delta.json').write_text(json.dumps(delta, indent=2) + '\n')
        print(mode, 'exports', len(after), 'missing', len(delta['missing']), 'added', len(delta['added']), flush=True)
        assert after == baseline, delta
        framework = dest / 'frameworks/Metal.framework/Versions/A'
        framework.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dest / 'Metal', framework / 'Metal')
        for link, target in ((framework.parent / 'Current', 'A'),
                             (framework.parents[1] / 'Metal', 'Versions/Current/Metal')):
            if not link.is_symlink():
                link.symlink_to(target)
    assert before == {str(p): sha(p) for p in inputs}
    dependencies = set()
    for depfile in OUT.glob('*/*.d'):
        dependencies.update(Path(p) for p in shlex.split(depfile.read_text().replace('\\\n', ' '))[1:])
    (OUT / 'provenance.json').write_text(json.dumps(dict(inputs_sha256=before,
        compilation_dependencies_sha256={str(p): sha(p) for p in sorted(dependencies)},
        tools_sha256={tool: sha(Path(shutil.which(tool))) for tool in ('clang++', 'ld64.lld', 'llvm-objdump')},
        compiler=run(['clang++', '--version'], 'compiler'),
        artifacts={str(p): sha(p) for p in OUT.glob('*/Metal')}, source_changes=False), indent=2) + '\n')


if __name__ == '__main__':
    main()
