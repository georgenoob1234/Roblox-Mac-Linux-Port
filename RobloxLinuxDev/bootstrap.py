#!/usr/bin/env python3
"""Fetch the pinned upstream inputs; keep downloads and build files in this folder."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent
CACHE = ROOT / '.downloads'


def run(*args, cwd=ROOT):
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def main():
    CACHE.mkdir(exist_ok=True)
    for name, spec in json.loads((ROOT / 'third_party/dependencies.json').read_text()).items():
        target = ROOT / spec['destination']
        if target.exists():
            if 'overlay' in spec:
                shutil.copytree(ROOT / spec['overlay'], target, dirs_exist_ok=True)
            print('Keeping existing dependency:', spec['destination'], flush=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        # Stage before renaming: interrupted downloads/builds never look complete.
        with tempfile.TemporaryDirectory(prefix='.bootstrap-', dir=target.parent) as temp:
            stage = Path(temp)
            if 'commit' in spec:
                checkout = stage / 'checkout'
                run('git', 'clone', '--no-checkout', spec['url'], checkout)
                run('git', 'checkout', '--detach', spec['commit'], cwd=checkout)
                run('git', 'submodule', 'update', '--init', '--recursive', cwd=checkout)
                run('git', 'apply', '--check', ROOT / spec['patch'], cwd=checkout)
                run('git', 'apply', ROOT / spec['patch'], cwd=checkout)
                checkout.rename(target)
                continue
            archive = CACHE / name
            if not archive.exists():
                part = archive.with_suffix('.part')
                run('curl', '--fail', '--location', '--retry', '3', '--output', part, spec['url'])
                part.rename(archive)
            digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
            if digest != spec['sha256']:
                raise SystemExit(f'Checksum mismatch: {archive}; remove it and retry, or review the changed upstream release')
            if name in ('appimagetool', 'appimage_runtime'):
                shutil.copy2(archive, target)
                target.chmod(0o755)
            elif name == 'darling_packages':
                unpacked = stage / 'debs'
                unpacked.mkdir()
                with zipfile.ZipFile(archive) as z:
                    members = [i for i in z.infolist() if i.filename.endswith('~noble_amd64.deb')]
                    if not members:
                        raise SystemExit('No noble amd64 packages in Darling archive')
                    for entry in members:
                        with z.open(entry) as src, (unpacked / Path(entry.filename).name).open('wb') as dst:
                            shutil.copyfileobj(src, dst)
                unpacked.rename(target)
            else:
                # bsdtar rejects traversal and writes through symlinks by default.
                # Darling's pinned SDK includes legitimate absolute symlinks.
                run('bsdtar', '--no-same-owner', '-xf', archive, '-C', stage)
                roots = list(stage.iterdir())
                if len(roots) != 1 or not roots[0].is_dir():
                    raise SystemExit(f'Unexpected archive layout: {name}')
                if 'overlay' in spec:
                    shutil.copytree(ROOT / spec['overlay'], roots[0], dirs_exist_ok=True)
                roots[0].rename(target)
        print('Ready:', spec['destination'], flush=True)


if __name__ == '__main__':
    main()
