#!/usr/bin/env python3
"""Fetch the pinned upstream inputs; keep downloads and build files in this folder."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent
CACHE = ROOT / '.downloads'


def run(*args, cwd=ROOT):
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def normalize_url(url):
    stripped = url.strip()
    if stripped.endswith('.git'):
        stripped = stripped[: -len('.git')]
    return stripped.rstrip('/')


class DependencyGitError(RuntimeError):
    def __init__(self, operation, target, stderr=''):
        self.operation = operation
        self.target = target
        self.stderr = stderr.strip()
        msg = f"git {operation} failed in {target}"
        if self.stderr:
            msg += f": {self.stderr}"
        super().__init__(msg)


def dependency_diff(target):
    """git diff --binary HEAD of a dependency checkout.

    Mirrors the comparison build-appimage.sh's packaging guard performs, so both
    always judge the exact same bytes. Raises DependencyGitError if Git fails.
    """
    target_path = Path(target).resolve()
    if not target_path.is_dir():
        raise DependencyGitError('rev-parse --git-dir', target, 'not a directory')
    git_env = {**os.environ, 'GIT_CEILING_DIRECTORIES': str(target_path.parent)}
    probe = subprocess.run(['git', 'rev-parse', '--git-dir'], cwd=target, env=git_env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if probe.returncode != 0:
        err = probe.stderr.decode(errors='replace').strip()
        raise DependencyGitError('rev-parse --git-dir', target, err)
    diff = subprocess.run(['git', 'diff', '--binary', 'HEAD'], cwd=target, env=git_env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if diff.returncode != 0:
        err = diff.stderr.decode(errors='replace').strip()
        raise DependencyGitError('diff --binary HEAD', target, err)
    return diff.stdout


def historical_patch_versions(patch_path):
    """Committed versions of a patch in the parent repository's history, as {sha: bytes}.

    The parent repository is the checkout this bootstrap lives in: it is the
    source of truth for what patch state a dependency was previously shipped
    with. HEAD plus all refs are walked, so versions stay reachable on the
    persistent CI checkout (detached at the built commit). A checkout without
    git history yields no versions, and refresh attempts then abort.
    """
    probe = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if probe.returncode != 0:
        return {}
    toplevel = Path(probe.stdout.decode().strip())
    try:
        rel = patch_path.relative_to(toplevel)
    except ValueError:
        return {}
    log = subprocess.run(['git', 'log', '--all', '--format=%H', '--', rel], cwd=toplevel,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if log.returncode != 0:
        return {}
    versions = {}
    for sha in log.stdout.decode().split():
        blob = subprocess.run(['git', 'show', f'{sha}:{rel}'], cwd=toplevel,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if blob.returncode == 0:
            versions[sha] = blob.stdout
    return versions


def apply_patch(target, patch_file, reverse=False):
    """git apply with a --check dry run first; a failed check leaves the tree untouched."""
    flags = ['--binary'] + (['--reverse'] if reverse else [])
    probe = subprocess.run(['git', 'apply', *flags, '--check', patch_file], cwd=target,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if probe.returncode != 0:
        print(f'bootstrap: git apply --check {" ".join(flags)} {patch_file} failed:\n'
              + probe.stderr.decode(errors='replace').strip(), file=sys.stderr, flush=True)
        return False
    applied = subprocess.run(['git', 'apply', *flags, patch_file], cwd=target,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return applied.returncode == 0


def reconcile_patched_dependency(name, spec, target):
    """Bring an existing patched Git dependency from a previously shipped patch to the
    current one — or abort clearly while leaving the checkout untouched.

    Persistent checkouts (the self-hosted CI runner) keep ignored dependency
    trees between builds while the tracked patch moves with the port. A stale
    dependency would otherwise only be caught by build-appimage.sh's guard,
    after the whole build. Policy, verified before anything is modified:

      * the checkout must be a Git repository whose origin matches the declared
        URL and whose HEAD is the pinned commit;
      * a checkout already reproducing the current patch byte-for-byte is kept
        as-is (build outputs and untracked files included);
      * otherwise its diff must be byte-identical to a patch version the parent
        repository committed — the reverse of that old patch restores the
        pristine pinned tree, the current patch is applied, and the result must
        again match the patch byte-for-byte, rolling back on any failure;
      * unknown local changes, a wrong HEAD/origin, or an unusable repository
        abort with a diagnostic; nothing is ever silently discarded.
    """
    where = spec['destination']
    patch_path = ROOT / spec['patch']
    if not patch_path.is_file():
        raise SystemExit(f'{name}: patch file is missing: {spec["patch"]}')

    def abort(message):
        raise SystemExit(f'{name}: {message}\n'
                         f'{name}: dependency left untouched at {where}; inspect and reconcile it '
                         f'manually while preserving build outputs and untracked files '
                         f'(never use git reset --hard, git clean, or deleting the dependency directory '
                         f'as automatic recovery methods)')

    try:
        current = dependency_diff(target)
    except DependencyGitError as err:
        detail = f': {err.stderr}' if err.stderr else ''
        abort(f'git {err.operation} failed on {where}{detail}')
    remote = subprocess.run(['git', 'remote', 'get-url', 'origin'], cwd=target,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    origin = remote.stdout.decode().strip()
    if remote.returncode != 0 or normalize_url(origin) != normalize_url(spec['url']):
        abort(f"origin '{origin or '(no origin remote)'}' does not match '{spec['url']}'")
    head_proc = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=target,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if head_proc.returncode != 0:
        err = head_proc.stderr.decode(errors='replace').strip()
        detail = f': {err}' if err else ''
        abort(f'git rev-parse HEAD failed on {where}{detail}')
    head = head_proc.stdout.decode().strip()
    if head != spec['commit']:
        abort(f'HEAD {head} is not the pinned commit {spec["commit"]}')

    patch = patch_path.read_bytes()
    if current == patch:
        return
    versions = historical_patch_versions(patch_path)
    origin_sha = next((sha for sha, blob in versions.items() if blob == current), None)
    if origin_sha is None:
        abort(f'local changes match no version of {spec["patch"]} committed to this repository; '
              f"inspect 'git -C {where} diff --binary HEAD' and export intended changes to the patch")

    print(f'Refreshing {name}: {where} still carries the {spec["patch"]} from {origin_sha[:12]}; '
          f'updating it in place (build outputs and untracked files are preserved)', flush=True)
    with tempfile.TemporaryDirectory(prefix='.bootstrap-refresh-') as temp:
        old_patch = Path(temp) / 'previous.patch'
        old_patch.write_bytes(current)
        if not apply_patch(target, old_patch, reverse=True):
            abort('cannot reverse the previously applied patch; the checkout is unchanged')
        if not apply_patch(target, patch_path):
            if apply_patch(target, old_patch):
                abort(f'{spec["patch"]} does not apply on top of the pinned commit; rolled back successfully')
            raise SystemExit(f'{name}: rolling back to the previous patch failed; inspect {where}')
        verification_diff = None
        verification_error = None
        try:
            verification_diff = dependency_diff(target)
        except DependencyGitError as err:
            verification_error = err

        if verification_error is not None or verification_diff != patch:
            if verification_error is not None:
                detail = f': {verification_error.stderr}' if verification_error.stderr else ''
                reason = f'git {verification_error.operation} failed on {where}{detail}'
            else:
                reason = f'refreshed {where} does not reproduce {spec["patch"]} byte-for-byte'

            if apply_patch(target, patch_path, reverse=True) and apply_patch(target, old_patch):
                abort(f'{reason}; rolled back successfully')
            raise SystemExit(f'{name}: {reason}; rolling back the failed refresh of {where} failed; inspect {where}')


def main():
    CACHE.mkdir(exist_ok=True)
    for name, spec in json.loads((ROOT / 'third_party/dependencies.json').read_text()).items():
        target = ROOT / spec['destination']
        if target.exists():
            if 'overlay' in spec:
                shutil.copytree(ROOT / spec['overlay'], target, dirs_exist_ok=True)
            if 'commit' in spec and 'patch' in spec:
                reconcile_patched_dependency(name, spec, target)
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
