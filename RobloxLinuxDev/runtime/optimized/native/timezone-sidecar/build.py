#!/usr/bin/env python3
"""Stage the checked libc control plus one patched localtime object; never install."""
from pathlib import Path
import fcntl
import hashlib
import json
import os
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
A = HERE.parents[2]
BUILD = A / 'src/darling-o2'
OUT = HERE / 'build'
SOURCE = A / 'src/darling/src/external/libc/stdtime/FreeBSD/localtime.c'
OBJECT = 'src/external/libc/stdtime/CMakeFiles/libc-stdtime.dir/FreeBSD/localtime.c.o'
TARGET = 'src/external/libc/libsystem_c.dylib'
SOURCE_SHA = 'a2f0941bd6c3bea0404d660e206ba7a4ebaf8bf4ae0dda135b87292bd45c9c65'
STOCK_SHA = 'b1a578ba173b705843e113f99348623f8923a57424ee2aac13c13617677ac6e3'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if not (BUILD / 'build.ninja').is_file() or not (BUILD / TARGET).is_file():
        raise SystemExit('Run RobloxLinuxDev/build.sh to configure Darling, then run:\n'
                         f'ninja -C {BUILD} {TARGET}\nNo install step is needed.')
    for folder in ('logs', 'tmp', 'control', 'candidate'):
        (OUT / folder).mkdir(parents=True, exist_ok=True)
    lock = (OUT / 'build.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    env = dict(os.environ, TMPDIR=str(OUT / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
    stock = A / 'optimized/darling/stock/darling-root/usr/lib/system/libsystem_c.dylib'
    if not stock.exists():
        stock = A / 'darling-root/usr/lib/system/libsystem_c.dylib'
    assert digest(SOURCE) == SOURCE_SHA, 'localtime source changed; rebase and test its patch'
    assert digest(stock) == STOCK_SHA, 'Stock libc changed; revalidate exports and dependencies'

    def run(command, label):
        command = list(map(str, command))
        (OUT / 'logs' / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=BUILD, env=env, capture_output=True, text=True, timeout=90)
        (OUT / 'logs' / (label + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise SystemExit('FAIL ' + label + '; see timezone-sidecar/build/logs/' + label + '.log')
        return result.stdout

    def recipe(target):
        command = subprocess.check_output(['ninja', '-C', str(BUILD), '-t', 'commands', target], text=True)
        args = shlex.split(command.splitlines()[-1])
        if args[:2] == [':', '&&'] and args[-2:] == ['&&', ':']:
            args = args[2:-2]
        assert args[0] == '/usr/bin/clang' and not {'&&', ';', '||', '>'}.intersection(args)
        return args

    link = recipe(TARGET)
    compile_command = recipe(OBJECT)
    assert link.count(OBJECT) == 1 and compile_command.count(str(SOURCE)) == 1
    # The rest of this library comes from the previously checked O2 tree.
    # Re-link the freshly built control and verify exports/dependencies below.
    # Absolute checkout paths affect Mach-O bytes; a historic binary hash is not portable.
    paths = {SOURCE, HERE / 'localtime.patch', HERE / 'build.py', stock,
             BUILD / 'build.ninja', BUILD / 'CMakeFiles/rules.ninja', BUILD / TARGET}
    for arg in link:
        if arg.endswith(('.o', '.dylib')):
            value = arg.rsplit(':', 1)[-1] if arg.startswith('-Wl,-dylib_file,') else arg
            path = Path(value)
            path = path if path.is_absolute() else BUILD / path
            if path.is_file():
                paths.add(path)
    before = {str(p): digest(p) for p in sorted(paths)}
    (OUT / 'inputs-before.json').write_text(json.dumps(before, indent=2) + '\n')
    control = OUT / 'control/libsystem_c.dylib'
    control_command = list(link)
    control_command[control_command.index('-o') + 1] = str(control)
    run(control_command, 'control-link')

    source = OUT / 'localtime.c'
    shutil.copy2(SOURCE, source)
    run(['patch', '--batch', '--fuzz=0', str(source), str(HERE / 'localtime.patch')], 'source-patch')
    obj = OUT / 'localtime.c.o'
    compile_command[1:1] = ['-iquote', str(SOURCE.parent)]
    for flag, value in (('-o', obj), ('-MF', obj.with_suffix('.o.d')), ('-MT', obj)):
        compile_command[compile_command.index(flag) + 1] = str(value)
    compile_command[compile_command.index(str(SOURCE))] = str(source)
    # Retain every original optimization flag, including the trailing -O0.
    run(compile_command, 'localtime-compile')
    candidate = OUT / 'candidate/libsystem_c.dylib'
    candidate_command = list(link)
    candidate_command[candidate_command.index(OBJECT)] = str(obj)
    candidate_command[candidate_command.index('-o') + 1] = str(candidate)
    run(candidate_command, 'candidate-link')

    def exports(path):
        return sorted(subprocess.check_output(['llvm-nm', '--arch=x86_64', '-gUj', str(path)], text=True).split())

    reference = exports(stock)
    for mode, library in (('control', control), ('candidate', candidate)):
        assert exports(library) == reference, 'Defined export mismatch: ' + mode
        for flag in ('--dylib-id', '--dylibs-used'):
            expected = subprocess.check_output(['llvm-objdump', '--macho', '--arch=x86_64', flag, str(stock)], text=True).splitlines()[1:]
            actual = subprocess.check_output(['llvm-objdump', '--macho', '--arch=x86_64', flag, str(library)], text=True).splitlines()[1:]
            assert actual == expected, 'Install identity/dependency mismatch: ' + mode
    assert before == {str(p): digest(p) for p in sorted(paths)}, 'Shared input changed during build'
    (OUT / 'provenance.json').write_text(json.dumps({
        'scope': 'Existing pinned O2 libc, replacing only its localtime object; broader than production stock libc',
        'inputs_sha256': before, 'patched_source_sha256': digest(source),
        'defined_exports': reference,
        'artifacts_sha256': {str(p): digest(p) for p in (stock, control, candidate)}
    }, indent=2) + '\n')
    print('PASS timezone libc control/candidate; stock exports and dependencies preserved; staged only')


if __name__ == '__main__':
    main()
