#!/usr/bin/env python3
"""Extract standard Arch packages privately; never bundle the build host's optimized libraries."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

def checked_records(output):
    records = []
    for line in output.splitlines():
        repo, name, arch, version, digest, url = line.split('|')
        if repo not in ('core', 'extra') or arch not in ('x86_64', 'any'):
            raise ValueError(f'Nonportable package: {repo}/{name} ({arch})')
        if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError(f'Missing SHA-256 for {name}')
        # Rolling mirrors can remove versions still named by the local sync DB.
        url = f'https://archive.archlinux.org/packages/{name[0]}/{name}/{url.rsplit("/", 1)[1]}'
        records.append(dict(name=name, version=version, arch=arch, sha256=digest, url=url))
    if not records:
        raise ValueError('No portable packages resolved')
    return records


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=True)
    # Updating the build host must not silently raise the release requirements.
    records = checked_records(Path(__file__).with_name('shader-packages.lock').read_text())
    manifest = json.dumps(records, sort_keys=True, indent=2) + '\n'
    prefix = directory / hashlib.sha256(manifest.encode()).hexdigest()[:16]
    if (prefix / 'packages.json').is_file() and (prefix / 'packages.json').read_text() == manifest:
        return prefix
    cache = directory / 'archives'
    cache.mkdir(exist_ok=True)

    def download(record):
        archive = cache / record['url'].rsplit('/', 1)[1]
        if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != record['sha256']:
            part = archive.with_suffix('.part')
            subprocess.run(['curl', '-fL', '--retry', '3', '--silent', '--show-error',
                            '-o', str(part), record['url']], check=True)
            if hashlib.sha256(part.read_bytes()).hexdigest() != record['sha256']:
                part.unlink()
                raise ValueError(f'Package checksum mismatch: {record["name"]}')
            part.replace(archive)
        return archive

    print(f'Preparing {len(records)} standard x86-64 packages (no host installation changes)', file=sys.stderr, flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        archives = list(pool.map(download, records))
    with tempfile.TemporaryDirectory(dir=directory) as tmp:
        stage = Path(tmp) / 'root'
        stage.mkdir()
        for archive in archives:
            subprocess.run(['bsdtar', '-xf', str(archive), '-C', str(stage),
                            '--no-same-owner', '--no-same-permissions'], check=True)
        (stage / 'packages.json').write_text(manifest)
        stage.rename(prefix)
    return prefix


if __name__ == '__main__':
    print(prepare(Path(__file__).resolve().parent / 'deps/portable'))
