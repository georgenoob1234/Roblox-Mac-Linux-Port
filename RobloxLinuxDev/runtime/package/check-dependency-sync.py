#!/usr/bin/env python3
"""Offline dependency-bootstrap checks: persistent patched checkouts.

The self-hosted CI runner keeps ignored dependency trees between builds while
the tracked patch moves with the port (sync-checkout.sh never touches them).
bootstrap.py must keep a checkout already matching the patch, move one still
carrying a previously committed patch version forward without losing build
outputs, and refuse to touch anything it cannot verify. Disposable local git
repositories only; the real dependency checkouts are never modified.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

DEV = Path(__file__).resolve().parents[2]
BOOTSTRAP = DEV / 'bootstrap.py'
assert 'reconcile_patched_dependency' in BOOTSTRAP.read_text()

GIT_ENV = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}


def git(*args, cwd):
    return subprocess.run(['git', *args], cwd=str(cwd), env=GIT_ENV, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def git_bytes(*args, cwd):
    return subprocess.run(['git', *args], cwd=str(cwd), env=GIT_ENV, check=True,
                          stdout=subprocess.PIPE).stdout


def commit(repo, message):
    git('add', '-A', cwd=repo)
    git('-c', 'user.name=check', '-c', 'user.email=check@example.invalid',
        'commit', '-q', '-m', message, cwd=repo)


def run_bootstrap(dev):
    return subprocess.run([sys.executable, '-B', str(dev / 'bootstrap.py')], cwd=str(dev),
                          env=GIT_ENV, capture_output=True, text=True)


def dependency_state(dep):
    """Everything bootstrap could judge or touch: diff vs HEAD, status, file bytes."""
    diff = git_bytes('diff', '--binary', 'HEAD', cwd=dep)
    status = git('status', '--porcelain', cwd=dep).stdout
    files = {str(p.relative_to(dep)): p.read_bytes()
             for p in sorted(dep.rglob('*'))
             if p.is_file() and '.git' not in p.parts}
    return diff, status, files


def packaging_guard(dep, pinned, patch_file):
    """The exact comparison build-appimage.sh performs before packaging."""
    assert git('rev-parse', 'HEAD', cwd=dep).stdout.strip() == pinned
    assert git_bytes('diff', '--binary', 'HEAD', cwd=dep) == patch_file.read_bytes()


with tempfile.TemporaryDirectory(prefix='roblox dependency sync ') as tmp:
    base = Path(tmp)

    # A stand-in upstream project: one pinned commit plus a later one.
    upstream = base / 'upstream'
    upstream.mkdir()
    git('init', cwd=upstream)
    (upstream / '.gitignore').write_text('target/\n')
    (upstream / 'source.txt').write_text('upstream base\n')
    (upstream / 'code.rs').write_text('fn main() {}\n')
    (upstream / 'data.bin').write_bytes(b'\x00\x01upstream\x02')
    (upstream / 'extra.txt').write_text('untouched by either patch\n')
    commit(upstream, 'base')
    pinned = git('rev-parse', 'HEAD', cwd=upstream).stdout.strip()
    (upstream / 'later.txt').write_text('later upstream change\n')
    commit(upstream, 'later')
    later = git('rev-parse', 'HEAD', cwd=upstream).stdout.strip()

    # Old and new patch versions, produced exactly like the tracked patches
    # (git diff --binary HEAD of the applied state). The new one strictly
    # extends the old one, as the metal2vulkan patch change did.
    scratch = base / 'scratch'
    git('clone', '-q', '--no-checkout', upstream, scratch, cwd=base)
    git('checkout', '-q', '--detach', pinned, cwd=scratch)
    (scratch / 'code.rs').write_text('fn main() {}\nfn patched() -> u32 { 1 }\n')
    (scratch / 'source.txt').write_text('patched base\n')
    old_patch = git_bytes('diff', '--binary', 'HEAD', cwd=scratch)
    (scratch / 'newmodule.rs').write_text('pub fn extra() -> u32 { 2 }\n')
    (scratch / 'data.bin').write_bytes(b'\x00\x01downstream\x02\xff')
    new_patch = git_bytes('diff', '--binary', 'HEAD', cwd=scratch)
    assert old_patch and new_patch and old_patch != new_patch

    # The parent checkout this bootstrap lives in, mirroring the real one: the
    # dependency directory and download cache are ignored, the patch is tracked.
    dev = base / 'parent' / 'dev'
    patch_file = dev / 'third_party/patches/fixture_dep.patch'
    patch_file.parent.mkdir(parents=True)
    (dev / '.gitignore').write_text('deps/\n.downloads/\n')
    (dev / 'third_party/dependencies.json').write_text(json.dumps({'fixture_dep': {
        'url': str(upstream),
        'commit': pinned,
        'destination': 'deps/fixture_dep',
        'patch': 'third_party/patches/fixture_dep.patch',
    }}))
    shutil.copy2(BOOTSTRAP, dev / 'bootstrap.py')
    patch_file.write_bytes(old_patch)
    parent = dev.parent
    git('init', cwd=parent)
    commit(parent, 'old patch')
    dep = dev / 'deps/fixture_dep'

    # Fresh install (existing behavior): clone at the pinned commit with the
    # current patch applied. Git dependencies install silently; the guard
    # comparison proves the resulting state.
    installed = run_bootstrap(dev)
    assert installed.returncode == 0, installed.stderr
    assert 'Keeping existing dependency' not in installed.stdout
    packaging_guard(dep, pinned, patch_file)

    # The persistent-checkout scenario that failed CI: sync-checkout.sh moves
    # the tracked tree (and with it the patch) to a newer commit while the
    # ignored dependency tree stays on the old patch. Build outputs and other
    # untracked files must survive the in-place refresh.
    output = dep / 'target/output.bin'
    output.parent.mkdir()
    output.write_bytes(b'precious build output')
    (dep / 'local-notes.txt').write_text('untracked note\n')
    patch_file.write_bytes(new_patch)
    commit(parent, 'new patch')
    git('checkout', '-q', '--detach', 'HEAD', cwd=parent)
    refreshed = run_bootstrap(dev)
    assert refreshed.returncode == 0, refreshed.stderr
    assert 'Refreshing fixture_dep' in refreshed.stdout, refreshed.stdout
    assert 'Keeping existing dependency: deps/fixture_dep' in refreshed.stdout
    packaging_guard(dep, pinned, patch_file)
    assert output.read_bytes() == b'precious build output'
    assert (dep / 'local-notes.txt').read_text() == 'untracked note\n'
    assert not list((dev / 'deps').glob('.bootstrap-*'))

    # A checkout already matching the patch is kept as-is.
    before = dependency_state(dep)
    kept = run_bootstrap(dev)
    assert kept.returncode == 0, kept.stderr
    assert 'Keeping existing dependency: deps/fixture_dep' in kept.stdout
    assert 'Refreshing' not in kept.stdout
    assert dependency_state(dep) == before

    # Unknown local changes abort with a diagnostic and stay byte-for-byte
    # intact (the modification lives in a file no patch version touches).
    with open(dep / 'extra.txt', 'a') as extra:
        extra.write('plus an unexpected local change\n')
    before = dependency_state(dep)
    unknown = run_bootstrap(dev)
    assert unknown.returncode != 0
    assert 'fixture_dep:' in unknown.stderr and 'left untouched' in unknown.stderr
    assert 'match no version' in unknown.stderr
    assert 'inspect and reconcile it manually while preserving build outputs and untracked files' in unknown.stderr
    assert 'never use git reset --hard, git clean, or deleting the dependency directory' in unknown.stderr
    assert 'remove the directory to re-bootstrap' not in unknown.stderr
    assert 'Traceback (most recent call last):' not in unknown.stderr
    assert dependency_state(dep) == before
    git('checkout', '--', 'extra.txt', cwd=dep)

    # A wrong HEAD aborts before anything is judged or touched.
    git('checkout', '-q', '--detach', later, cwd=dep)
    before = dependency_state(dep)
    wrong_head = run_bootstrap(dev)
    assert wrong_head.returncode != 0
    assert 'is not the pinned commit' in wrong_head.stderr
    assert 'left untouched' in wrong_head.stderr
    assert 'inspect and reconcile it manually while preserving build outputs and untracked files' in wrong_head.stderr
    assert 'never use git reset --hard, git clean, or deleting the dependency directory' in wrong_head.stderr
    assert 'remove the directory to re-bootstrap' not in wrong_head.stderr
    assert 'Traceback (most recent call last):' not in wrong_head.stderr
    assert dependency_state(dep) == before
    git('checkout', '-q', '--detach', pinned, cwd=dep)

    # A foreign origin aborts the same way.
    git('remote', 'set-url', 'origin', str(base / 'somewhere-else'), cwd=dep)
    before = dependency_state(dep)
    wrong_origin = run_bootstrap(dev)
    assert wrong_origin.returncode != 0
    assert "does not match" in wrong_origin.stderr
    assert 'left untouched' in wrong_origin.stderr
    assert 'inspect and reconcile it manually while preserving build outputs and untracked files' in wrong_origin.stderr
    assert 'never use git reset --hard, git clean, or deleting the dependency directory' in wrong_origin.stderr
    assert 'remove the directory to re-bootstrap' not in wrong_origin.stderr
    assert 'Traceback (most recent call last):' not in wrong_origin.stderr
    assert dependency_state(dep) == before
    git('remote', 'set-url', 'origin', str(upstream), cwd=dep)

    # A broken Git checkout (invalid HEAD object) aborts with an actionable diagnostic
    # and preserved stderr, without an unhandled traceback or unintended modification.
    head_file = dep / '.git/HEAD'
    saved_head = head_file.read_bytes()
    head_file.write_text('0123456789abcdef0123456789abcdef01234567\n')
    before_files = {str(p.relative_to(dep)): p.read_bytes()
                    for p in sorted(dep.rglob('*'))
                    if p.is_file() and '.git' not in p.parts}
    broken_head = run_bootstrap(dev)
    assert broken_head.returncode != 0
    assert 'Traceback (most recent call last):' not in broken_head.stderr
    assert 'fixture_dep:' in broken_head.stderr
    assert 'git diff --binary HEAD failed' in broken_head.stderr
    assert 'deps/fixture_dep' in broken_head.stderr
    assert 'bad object HEAD' in broken_head.stderr
    assert 'left untouched at deps/fixture_dep' in broken_head.stderr
    assert 'inspect and reconcile it manually while preserving build outputs and untracked files' in broken_head.stderr
    assert 'never use git reset --hard, git clean, or deleting the dependency directory' in broken_head.stderr
    assert 'remove the directory to re-bootstrap' not in broken_head.stderr
    assert output.read_bytes() == b'precious build output'
    assert (dep / 'local-notes.txt').read_text() == 'untracked note\n'
    after_files = {str(p.relative_to(dep)): p.read_bytes()
                   for p in sorted(dep.rglob('*'))
                   if p.is_file() and '.git' not in p.parts}
    assert after_files == before_files
    head_file.write_bytes(saved_head)

    # A damaged Git repository (.git missing or invalid) fails rev-parse --git-dir cleanly.
    git_dir = dep / '.git'
    git_dir_backup = dep / '.git_backup'
    git_dir.rename(git_dir_backup)
    before_files = {str(p.relative_to(dep)): p.read_bytes()
                    for p in sorted(dep.rglob('*'))
                    if p.is_file() and '.git' not in p.parts}
    broken_git = run_bootstrap(dev)
    assert broken_git.returncode != 0
    assert 'Traceback (most recent call last):' not in broken_git.stderr
    assert 'fixture_dep:' in broken_git.stderr
    assert 'git rev-parse --git-dir failed' in broken_git.stderr
    assert 'deps/fixture_dep' in broken_git.stderr
    assert 'left untouched at deps/fixture_dep' in broken_git.stderr
    assert 'inspect and reconcile it manually while preserving build outputs and untracked files' in broken_git.stderr
    assert 'never use git reset --hard, git clean, or deleting the dependency directory' in broken_git.stderr
    assert 'remove the directory to re-bootstrap' not in broken_git.stderr
    assert output.read_bytes() == b'precious build output'
    assert (dep / 'local-notes.txt').read_text() == 'untracked note\n'
    after_files = {str(p.relative_to(dep)): p.read_bytes()
                   for p in sorted(dep.rglob('*'))
                   if p.is_file() and '.git' not in p.parts}
    assert after_files == before_files
    git_dir_backup.rename(git_dir)

    # Verification failure during refresh: roll back to the previously applied patch.
    # Set dep back to the old patch to prepare for a refresh attempt.
    git('checkout', '-q', '--detach', pinned, cwd=dep)
    git('checkout', '-q', '.', cwd=dep)
    git('clean', '-q', '-f', '-d', cwd=dep)
    # Re-apply the old patch to dep and recreate the sentinels.
    patch_temp = base / 'old.patch'
    patch_temp.write_bytes(old_patch)
    git('apply', '--binary', str(patch_temp), cwd=dep)
    output.write_bytes(b'precious build output')
    (dep / 'local-notes.txt').write_text('untracked note\n')
    assert git_bytes('diff', '--binary', 'HEAD', cwd=dep) == old_patch

    # Create a patch that applies cleanly but whose binary diff will not match byte-for-byte
    # due to a trailing comment.
    bad_patch = new_patch + b'# comment not produced by git diff\n'
    patch_file.write_bytes(bad_patch)
    commit(parent, 'bad patch with comment')
    before_rollback = dependency_state(dep)
    failed_refresh = run_bootstrap(dev)
    assert failed_refresh.returncode != 0
    assert 'Traceback (most recent call last):' not in failed_refresh.stderr
    assert 'fixture_dep:' in failed_refresh.stderr
    assert 'does not reproduce third_party/patches/fixture_dep.patch byte-for-byte' in failed_refresh.stderr
    assert 'rolled back successfully' in failed_refresh.stderr
    assert 'left untouched at deps/fixture_dep' in failed_refresh.stderr
    assert dependency_state(dep) == before_rollback
    assert output.read_bytes() == b'precious build output'
    assert (dep / 'local-notes.txt').read_text() == 'untracked note\n'

    # Back to the healthy state: bootstrap succeeds and the dependency again
    # reproduces the current patch exactly.
    patch_file.write_bytes(new_patch)
    commit(parent, 'restore clean new patch')
    refreshed_final = run_bootstrap(dev)
    assert refreshed_final.returncode == 0, refreshed_final.stderr
    assert 'Refreshing fixture_dep' in refreshed_final.stdout
    packaging_guard(dep, pinned, patch_file)
    assert output.read_bytes() == b'precious build output'
    assert (dep / 'local-notes.txt').read_text() == 'untracked note\n'

    # Subsequent run: matching patch is kept as-is.
    final = run_bootstrap(dev)
    assert final.returncode == 0, final.stderr
    assert 'Refreshing' not in final.stdout
    packaging_guard(dep, pinned, patch_file)
print('PASS dependency bootstrap: fresh install, stale committed patch refreshed in place with build '
      'outputs preserved, matching patch untouched, unknown changes / wrong HEAD / wrong origin / '
      'broken git abort intact, refresh verification failure rolled back')
