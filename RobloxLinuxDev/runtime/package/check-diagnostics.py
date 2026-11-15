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
import time

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
    shutil.copy2(root / 'package/bootstrap.sh', release)
    shutil.copy2(root / 'package/bootstrap.py', release)
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
    debug = subprocess.run(['sh', release / 'run.sh', '--debug'], env=env, capture_output=True, text=True)
    assert debug.returncode != 0, debug.stderr
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
    # GPU driver faults the kernel logged during a debug run are kept next to its log.
    tools = folder / 'tools'
    tools.mkdir()
    journal = tools / 'journalctl'
    kernel_log = folder / 'kernel.log'
    kernel_log.write_text(
        '2026-11-14T21:20:49+0300 kernel: [UFW BLOCK] IN=eth0 OUT= SRC=192.0.2.1 DST=224.0.0.251 LEN=32\n'
        '2026-11-14T21:21:13+0300 kernel: NVRM: Xid (PCI:0000:2b:00): 32, pid=358557, name=mldr, channel 0x00000035 intr 00800000\n'
        '2026-11-14T21:21:14+0300 kernel: amdgpu 0000:03:00.0: amdgpu: GPU reset begin!\n'
        '2026-11-14T21:21:15+0300 kernel: [drm:amdgpu_job_timedout [amdgpu]] *ERROR* ring gfx_0.0.0 timeout, signaled seq=1, emitted seq=3\n'
        '2026-11-14T21:21:16+0300 kernel: i915 0000:00:02.0: [drm] GPU HANG: ecode 9:1:85dffffb, in mldr [1234]\n'
        '2026-11-14T21:21:17+0300 kernel: usb 1-2: new high-speed USB device number 5 using xhci_hcd\n')
    journal.write_text(f'#!/bin/sh\nprintf \'%s\\n\' "$@" > "{folder}/journal-args"\nexec cat "{kernel_log}"\n')
    journal.chmod(0o755)
    child.write_text('#!/bin/sh\nkill -ABRT $$\n')
    before = time.time()
    result = subprocess.run([sys.executable, script, '--debug', runtime, data, child],
                            env={**env, 'PATH': f'{tools}:{env["PATH"]}'}, capture_output=True, text=True)
    assert result.returncode == 128 + signal.SIGABRT, result.stderr
    assert 'GPU driver faults in the kernel log during this run: 4 (gpu-faults.txt)' in result.stderr, result.stderr
    newest = max((data / 'diagnostics').glob('*/runtime.log'), key=lambda path: path.stat().st_mtime).parent
    faults = (newest / 'gpu-faults.txt').read_text().splitlines()
    assert len(faults) == 4 and 'Xid' in faults[0] and 'GPU HANG' in faults[3] and not any('UFW' in line or 'usb' in line for line in faults)
    assert (newest / 'gpu-faults.txt').stat().st_mode & 0o777 == 0o600
    arguments = (folder / 'journal-args').read_text().split()
    since = [value for value in arguments if value.startswith('--since=@')]
    assert '-k' in arguments and '--no-hostname' in arguments and len(since) == 1 and before - 2 <= int(since[0][9:]) <= time.time()
    # A kernel log that cannot be read, or one without faults, leaves no file and no line.
    for script_text in ('#!/bin/sh\nexit 1\n', '#!/bin/sh\necho "2026-11-14T21:21:17+0300 kernel: usb 1-2: new device"\n'):
        journal.write_text(script_text)
        result = subprocess.run([sys.executable, script, '--debug', runtime, data, child],
                                env={**env, 'PATH': f'{tools}:{env["PATH"]}'}, capture_output=True, text=True)
        assert result.returncode == 128 + signal.SIGABRT and 'GPU driver faults' not in result.stderr
    assert len(list((data / 'diagnostics').glob('*/gpu-faults.txt'))) == 1
    # Old diagnostics folders are pruned: the ten newest stay. Only this script's own folders are
    # touched; a link is never followed.
    logs = data / 'diagnostics'
    ours = lambda: sorted(path.name for path in logs.iterdir() if path.name[:8].isdigit() and path.is_dir() and not path.is_symlink())
    for path in [logs / name for name in ours()]:
        shutil.rmtree(path)
    for index in range(12):
        old = logs / f'202001{index + 1:02d}T000000Z-aaaaaaaa'
        (old / 'shaders').mkdir(parents=True)
        (old / 'shaders/shader.out').write_text('old')
    outside = folder / 'outside'
    outside.mkdir()
    (outside / 'precious').write_text('keep')
    (logs / '20190101T000000Z-linkaaaa').symlink_to(outside)
    (logs / 'my notes').mkdir()
    (logs / '20190101T000000Z-file.txt').write_text('keep')
    run = lambda extra: subprocess.run([sys.executable, script, '--diagnose', runtime, data, child],
                                       env={**env, **extra}, capture_output=True, text=True)
    result = run({})
    assert result.returncode == 0, result.stderr
    kept = ours()
    assert len(kept) == 10 and kept[0] == '20200104T000000Z-aaaaaaaa' and kept[-1][:4] >= '2026', kept
    assert (logs / kept[-1] / 'system.txt').is_file() and (logs / kept[0] / 'shaders/shader.out').read_text() == 'old'
    assert (outside / 'precious').read_text() == 'keep' and (logs / '20190101T000000Z-linkaaaa').is_symlink()
    assert (logs / 'my notes').is_dir() and (logs / '20190101T000000Z-file.txt').read_text() == 'keep'
    # ROBLOX_MAC_DIAGNOSTICS_KEEP sets the number; 0 keeps everything; nonsense means the default.
    assert run({'ROBLOX_MAC_DIAGNOSTICS_KEEP': '0'}).returncode == 0 and len(ours()) == 11
    assert run({'ROBLOX_MAC_DIAGNOSTICS_KEEP': 'many'}).returncode == 0 and len(ours()) == 10
    assert run({'ROBLOX_MAC_DIAGNOSTICS_KEEP': '3'}).returncode == 0 and len(ours()) == 3
    assert run({'ROBLOX_MAC_DIAGNOSTICS_KEEP': '1'}).returncode == 0 and len(ours()) == 1
    # A folder that cannot be listed never stops a run.
    assert diag.prune(folder / 'missing', None) is None
    (data / 'config.env').unlink()
    # Explicit diagnostic arguments are forwarded directly to the AppImage.
    actual = subprocess.check_output(['sh', release / 'run.sh', '--debug', '--show-config'], env=env, text=True).splitlines()
    assert any(line.startswith('CONFIG=') for line in actual) and any(line.startswith('SHADER_CACHE=') for line in actual)
print('PASS diagnostics: signals, namespace/tool checks, no-launch mode, private logs, exit status, shader log retention, GPU faults, pruning, arguments')
