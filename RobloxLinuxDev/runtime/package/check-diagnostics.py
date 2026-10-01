#!/usr/bin/env python3
"""Offline diagnostic checks: signal details, exit status, private logs and launcher arguments."""
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
script = root / 'scripts/diagnostics.py'
spec = importlib.util.spec_from_file_location('diagnostics', script)
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)
assert 'signal 11 (SIGSEGV)' in diag.probe('crash',
    [sys.executable, '-c', 'import os,signal; os.kill(os.getpid(),signal.SIGSEGV)'], shader=True)
assert 'unavailable' in diag.probe('missing', ['/nonexistent/roblox-diagnostic-tool'])

with tempfile.TemporaryDirectory(prefix='roblox diagnostics é ') as tmp:
    folder = Path(tmp)
    runtime = folder / 'runtime'
    release = folder / 'release'
    data = release / 'DO_NOT_SHARE'
    (runtime / 'usr/bin').mkdir(parents=True)
    (runtime / 'scripts').mkdir()
    (runtime / 'bin').mkdir()
    release.mkdir()
    shutil.copy2(script, runtime / 'scripts')
    shutil.copy2(root / 'bin/roblox-mac', runtime / 'bin')
    shutil.copy2(root / 'package/run.sh', release)
    shutil.copy2(root / 'package/update-roblox.sh', release)
    for name in ('llvm-dis', 'spirv-val'):
        tool = runtime / 'usr/bin' / name
        tool.write_text('#!/bin/sh\nkill -ILL $$\n' if name == 'llvm-dis' else '#!/bin/sh\necho fake-version\n')
        tool.chmod(0o755)
    helper = runtime / 'optimized/ui/libroblox-wayland.so'
    helper.parent.mkdir(parents=True)
    helper.write_text('broken native UI library')
    shutil.copytree(root / 'metal', runtime / 'metal', ignore=shutil.ignore_patterns('out', '__pycache__'))
    translator = runtime / 'usr/bin/metal2vulkan'
    translator.write_text('#!/bin/sh\n[ "$LD_LIBRARY_PATH" = "$ROBLOX_MAC_SHADER_LIBRARY_PATH" ] || exit 2\necho "metal2vulkan: llvm-dis killed by signal [no verdict]:" >&2\nexit 1\n')
    translator.chmod(0o755)
    grep = runtime / 'usr/bin/grep'
    grep.write_text('#!/bin/sh\n[ "${LD_LIBRARY_PATH:-}" != "$ROBLOX_MAC_SHADER_LIBRARY_PATH" ] || exit 2\nexec ' + shutil.which('grep') + ' "$@"\n')
    grep.chmod(0o755)
    pack = folder / 'client/Contents/Resources/shaders/shaders_metal_osx.pack'
    pack.parent.mkdir(parents=True)
    blob = bytearray(92)
    blob[:4] = b'MTLB'
    struct.pack_into('<Q', blob, 16, len(blob))
    struct.pack_into('<QQ', blob, 72, 88, 4)
    pack.write_bytes(blob)
    launcher = runtime / 'bin/roblox-mac'
    image = release / 'RobloxLinux.AppImage'
    image.write_text('#!/bin/sh\nexec "$APPDIR/bin/roblox-mac" "$@"\n')
    image.chmod(0o755)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('ROBLOX_', 'APPIMAGE', 'APPDIR', 'METAL2VULKAN_'))}
    env.update(APPDIR=str(runtime), APPIMAGE=str(image), TEST_SECRET='DO-NOT-COLLECT-THIS')
    env['ROBLOX_MAC_SHADER_LIBRARY_PATH'] = str(runtime / 'browser/lib')
    translated = subprocess.run([runtime / 'metal/translate-all.sh', folder / 'client'],
        env={**env, 'PATH': str(runtime / 'usr/bin') + ':' + env['PATH'],
             'ROBLOX_MAC_SHADER_BUILD': str(folder / 'shader-build')}, capture_output=True, text=True)
    assert translated.returncode == 0 and 'signal 4 (SIGILL)' in translated.stdout, translated.stderr
    diagnostic = subprocess.run(['sh', release / 'run.sh', '--diagnose'], env=env, capture_output=True, text=True)
    assert diagnostic.returncode == 0, diagnostic.stderr
    assert 'signal 4 (SIGILL)' in diagnostic.stdout and 'MemTotal:' in diagnostic.stdout
    assert 'Native UI library load: exit 1' in diagnostic.stdout and 'OSError:' in diagnostic.stdout
    assert 'DO-NOT-COLLECT-THIS' not in diagnostic.stdout
    reports = list((data / 'diagnostics').glob('*/system.txt'))
    assert len(reports) == 1 and reports[0].stat().st_mode & 0o777 == 0o600
    assert reports[0].parent.stat().st_mode & 0o777 == 0o700
    assert not (release / 'RobloxVersion').exists(), 'Diagnosis must not download or launch the client'
    # Debug must preserve the launcher's failure status and capture early config errors.
    (data / 'config.env').write_text('ROBLOX_MAC_SWAP_INTERVAL=invalid\n')
    debug = subprocess.run(['sh', release / 'run.sh'], env=env, capture_output=True, text=True)
    assert debug.returncode == 2, debug.stderr
    log = next((data / 'diagnostics').glob('*/runtime.log')).read_text()
    assert 'ROBLOX_MAC_SWAP_INTERVAL must be' in log and 'Runtime result: exit 2' in log
    # Simulate shader preparation; text logs survive staging cleanup, binary inputs do not get copied.
    stage = data / 'stage'
    source = stage / 'metal-build/spv'
    source.mkdir(parents=True)
    (source / '0000.out').write_text('llvm-dis killed by signal')
    (source / '0000.time').write_text('0.1 100')
    (source / '0000.spv').write_bytes(b'private shader')
    child = folder / 'child'
    child.write_text('#!/bin/sh\necho child-stdout\necho child-stderr >&2\nexit 7\n')
    child.chmod(0o755)
    result = subprocess.run([sys.executable, script, '--debug', runtime, data, child, '--prepare-shaders'],
                            env={**env, 'ROBLOX_MAC_DATA': str(stage)}, capture_output=True, text=True)
    assert result.returncode == 7 and 'child-stdout' in result.stdout and 'child-stderr' in result.stdout
    saved = next((data / 'diagnostics').glob('*/shaders'))
    shutil.rmtree(stage)
    assert {p.name for p in saved.iterdir()} == {'0000.out', '0000.time'}
    # A directly signalled child is named in the log and becomes a conventional shell status.
    child.write_text('#!/bin/sh\nkill -TERM $$\n')
    result = subprocess.run([sys.executable, script, '--debug', runtime, data, child], env=env, capture_output=True, text=True)
    assert result.returncode == 128 + signal.SIGTERM and 'SIGTERM' in result.stderr
    child.write_text('#!/bin/sh\necho ready\nexec sleep 60\n')
    running = subprocess.Popen([sys.executable, script, '--debug', runtime, data, child],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert running.stdout.readline() == 'ready\n'
    running.terminate()
    _, error = running.communicate(timeout=5)
    assert running.returncode == 128 + signal.SIGTERM and 'SIGTERM' in error
    # The shell launcher must forward arguments unchanged and only insert --debug once.
    image.unlink()
    image.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
    image.chmod(0o755)
    for args, expected in ((['--place-id', '12 34'], ['--debug', '--place-id', '12 34']),
                           (['--debug', '--show-config'], ['--debug', '--show-config'])):
        actual = subprocess.check_output(['sh', release / 'run.sh', *args], env=env, text=True).splitlines()
        assert actual == expected, actual
print('PASS diagnostics: signals, namespace/tool checks, no-launch mode, private logs, exit status, shader log retention, arguments')
