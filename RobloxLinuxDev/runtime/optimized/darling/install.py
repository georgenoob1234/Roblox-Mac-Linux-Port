#!/usr/bin/env python3
"""Install the -O2 Darling build over the stock (-O0) Debian runtime.

The upstream Debian packaging configures CMake without a build type, so every
stock library in darling-root is unoptimized. build.sh compiles the same
Darling source (plus this project's source fixes) at -O2 into a staging tree;
this script swaps in its libraries, bundles, dyld, mldr and darlingserver.

Every stock file is backed up once under stock/ before it is replaced, so
`install.py --restore` returns darling-root to the Debian runtime exactly.
Guest executables (shells, daemons) are left alone: the game does not run in
them. The project's own builds (CoreAudio, CoreML stub, Metal/Indium/Iridium)
are never touched.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
A = HERE.parents[1]
ROOT = A / 'darling-root'
STAGE = A / 'src/darling-o2-stage'
GUEST = STAGE / 'usr/libexec/darling'
BACKUP = HERE / 'stock'
MANIFEST = HERE / 'manifest.json'
FOUNDATION = 'System/Library/Frameworks/Foundation.framework/Versions/C/Foundation'
SKIP = (FOUNDATION, 'System/Library/Frameworks/CoreAudio.framework', 'System/Library/Components/CoreAudio.component',
        'System/Library/Frameworks/CoreML.framework', 'System/Library/Frameworks/Metal',
        'usr/lib/darling/',
        # Clang combines this library's sin/cos wrappers into a recursive
        # __sincos*_stret call at O2; retain the working stock math library.
        'usr/lib/system/libsystem_m.dylib',
        # ELF wrappers are regenerated against the build host's library
        # headers; keep the Debian ones, which match what the prefix expects.
        'usr/lib/native/')
MACHO_KINDS = ('DYLIB', 'BUNDLE', 'DYLINKER')


def output(*command):
    return subprocess.run(command, capture_output=True, text=True, errors='replace').stdout


def is_macho(path):
    with open(path, 'rb') as f:
        return f.read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')


def kind(path):
    header = output('llvm-objdump', '--macho', '--private-header', '--arch=x86_64', str(path))
    return next((k for k in MACHO_KINDS if k in header), None)


def symbols(path, *flags):
    return set(output('llvm-nm', *flags, '--arch=x86_64', '-j', str(path)).split())


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pairs():
    """(installed path, rebuilt path) for everything this script manages."""
    for built in sorted(GUEST.rglob('*')):
        if not built.is_file() or built.is_symlink():
            continue
        rel = built.relative_to(GUEST).as_posix()
        stock = ROOT / rel
        if rel.startswith(SKIP) or not stock.is_file() or stock.is_symlink():
            continue
        if rel == 'usr/libexec/darling/mldr' or (is_macho(built) and kind(stock)):
            yield stock, built
    # The O2 Foundation build breaks Roblox Home (its request returns HTTP 500).
    # Keep the working stock framework while optimizing the rest of the runtime.
    yield ROOT / FOUNDATION, original(ROOT / FOUNDATION)
    yield A / 'darlingserver', STAGE / 'usr/bin/darlingserver'
    yield A / 'darling-cli', STAGE / 'usr/bin/darling'


def original(installed):
    saved = BACKUP / installed.relative_to(A)
    return saved if saved.exists() else installed


def verify(targets):
    """Refuse an install that changes a library's identity or drops a symbol
    some other image still imports (the project's builds, the client, stock
    files this script does not replace)."""
    lost, provided = set(), set()
    for installed, built in targets:
        if not is_macho(built):
            continue
        before = original(installed)
        for flag in ('--dylib-id', '--dylibs-used'):
            if output('llvm-objdump', '--macho', flag, '--arch=x86_64', str(before)).splitlines()[1:] != \
                    output('llvm-objdump', '--macho', flag, str(built)).splitlines()[1:]:
                raise SystemExit(f'FAIL {flag} changed: {installed}')
        lost |= symbols(before, '-gU') - symbols(built, '-gU')
        provided |= symbols(built, '-gU')
    managed = {installed for installed, _ in targets}
    others = [p for p in ROOT.rglob('*') if p.is_file() and not p.is_symlink() and p not in managed and is_macho(p)]
    others += [p for d in ('optimized', 'shims') for p in (A / d).rglob('*.dylib')
               if is_macho(p) and not {'build', 'tmp', 'stock'} & set(p.parts)]
    client = Path(os.environ.get('ROBLOX_MAC_APP', A.parent / '.build/client/RobloxPlayer.app'))
    others += [p for p in client.rglob('*') if p.is_file() and is_macho(p)]
    for p in others:
        provided |= symbols(p, '-gU')
    missing = lost - provided
    for p in others:
        needed = symbols(p, '-u') & missing
        if needed:
            raise SystemExit(f'FAIL {p} imports symbols the -O2 build no longer exports: {sorted(needed)[:5]}')


def install():
    targets = list(pairs())
    verify(targets)
    record = {}
    for installed, built in targets:
        saved = BACKUP / installed.relative_to(A)
        if not saved.exists():
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(installed, saved)
        temporary = installed.with_name(installed.name + '.o2-new')
        shutil.copy2(built, temporary)
        temporary.chmod(saved.stat().st_mode & 0o7777)
        temporary.replace(installed)
        record[str(installed.relative_to(A))] = {'stock': sha256(saved), 'installed': sha256(installed)}
    MANIFEST.write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')
    print(f'PASS installed {len(record)} -O2 Darling files; stock copies in {BACKUP}')


def restore():
    count = 0
    for saved in sorted(p for p in BACKUP.rglob('*') if p.is_file()):
        installed = A / saved.relative_to(BACKUP)
        temporary = installed.with_name(installed.name + '.o2-restore')
        shutil.copy2(saved, temporary)
        temporary.replace(installed)
        count += 1
    print(f'PASS restored {count} stock Darling files')


if __name__ == '__main__':
    restore() if sys.argv[1:] == ['--restore'] else install()
