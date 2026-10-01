#!/usr/bin/env python3
"""Local hardware/tool report and opt-in runtime logging; never collect session files."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError as error:
        return f'unavailable ({error.strerror})'


def status(code):
    return f'signal {-code} ({signal.Signals(-code).name})' if code < 0 else f'exit {code}'


def shader_limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_AS, (500 * 1024**2,) * 2)


def probe(label, command, *, env=None, shader=False):
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True,
                                errors='replace', timeout=20,
                                preexec_fn=shader_limits if shader else None)
        return f'{label}: {status(result.returncode)}\n{result.stdout}{result.stderr}'.rstrip()
    except subprocess.TimeoutExpired:
        return f'{label}: timed out after 20 seconds'
    except OSError as error:
        return f'{label}: unavailable ({error.strerror})'


def report(root):
    lines = [f'UTC: {datetime.now(timezone.utc).isoformat()}',
             f'Runtime build: {read(root / "BUILD-ID")}',
             f'Kernel: {platform.release()} ({platform.machine()})',
             f'libc: {" ".join(platform.libc_ver())}', f'Python: {platform.python_version()}']
    try:
        lines.append(f'OS: {platform.freedesktop_os_release().get("PRETTY_NAME", "unknown")}')
    except OSError:
        lines.append('OS: unavailable')
    cpu = dict(line.split(':', 1) for line in read('/proc/cpuinfo').split('\n\n')[0].splitlines() if ':' in line)
    cpu = {key.strip(): value.strip() for key, value in cpu.items()}
    lines += [f'CPU: {cpu.get("model name", "unknown")}', f'CPU flags: {cpu.get("flags", "unknown")}',
              f'Available CPUs: {len(os.sched_getaffinity(0))}']
    lines += [line for line in read('/proc/meminfo').splitlines()
              if line.split(':')[0] in ('MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree')]
    lines.append(f'Display sockets: Wayland={bool(os.environ.get("WAYLAND_DISPLAY"))}, X11={bool(os.environ.get("DISPLAY"))}')
    for device in sorted(Path('/sys/class/drm').glob('card[0-9]*/device')):
        if not device.parent.name.removeprefix('card').isdigit():
            continue
        lines.append(f'GPU: vendor={read(device / "vendor")} device={read(device / "device")} driver={(device / "driver").resolve().name}')
    for name in ('kernel/unprivileged_userns_clone', 'user/max_user_namespaces', 'kernel/apparmor_restrict_unprivileged_userns'):
        lines.append(f'{name}: {read(Path("/proc/sys") / name)}')
    lines.append(probe('User namespaces', ['unshare', '-Ur', 'true']))
    lines.append('Shader limits: 500 MiB address space/RSS per translation, 20 seconds; parallelism=nproc')
    env = dict(os.environ, PATH=f'{root}/usr/bin:{os.environ.get("PATH", "")}',
               LD_LIBRARY_PATH=f'{root}/browser/lib' + (':' + os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else ''))
    for tool in ('llvm-dis', 'spirv-val'):
        binary = os.environ.get('METAL2VULKAN_' + tool.upper().replace('-', '_'), tool)
        lines.append(probe(f'{tool} --version (shader limits)', [binary, '--version'], env=env, shader=True))
    helper = root / 'optimized/ui/libroblox-wayland.so'
    if helper.is_file():
        lines.append(probe('Native UI library load', [sys.executable, '-c',
            'import ctypes,sys; ctypes.CDLL(sys.argv[1]); print("UI library loaded successfully")', str(helper)], env=env))
        # Probe drivers after the UI, matching the app's library loading order.
        # GLVND otherwise hides dlopen errors behind EGL_NO_DISPLAY.
        for directory in ('/etc/glvnd/egl_vendor.d', '/usr/share/glvnd/egl_vendor.d'):
            for manifest in sorted(Path(directory).glob('*.json')):
                try:
                    library = json.loads(manifest.read_text())['ICD']['library_path']
                    lines.append(probe(f'EGL driver {manifest.name}', [sys.executable, '-c',
                        'import ctypes,sys; ctypes.CDLL(sys.argv[1]); ctypes.CDLL(sys.argv[2]); print("EGL driver loaded successfully")',
                        str(helper), library], env=env))
                except (OSError, ValueError, KeyError, TypeError) as error:
                    lines.append(f'EGL driver {manifest.name}: {error}')
    return '\n'.join(lines) + '\n'


def main():
    mode = sys.argv[1]
    if mode == '--llvm-check':
        env = dict(os.environ)
        if libraries := env.get('ROBLOX_MAC_SHADER_LIBRARY_PATH'):
            env['LD_LIBRARY_PATH'] = libraries
        print(probe('llvm-dis retry on a failed shader (shader limits)',
                    [os.environ.get('METAL2VULKAN_LLVM_DIS', 'llvm-dis'), sys.argv[2], '-o', '/dev/null'], env=env, shader=True))
        return 0
    root, data, launcher = map(Path, sys.argv[2:5])
    args = sys.argv[5:]
    os.umask(0o077)
    data.mkdir(parents=True, exist_ok=True)
    data.chmod(0o700)
    logs = data / 'diagnostics'
    logs.mkdir(exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-'), dir=logs))
    print(f'Diagnostics: {folder}', file=sys.stderr, flush=True)
    summary = report(root)
    (folder / 'system.txt').write_text(summary)
    if mode == '--diagnose':
        print(summary, end='')
        return 0
    start = time.monotonic()
    env = dict(os.environ)
    env.setdefault('VK_LOADER_DEBUG', 'error,warn')
    with (folder / 'runtime.log').open('wb') as log:
        process = subprocess.Popen([str(launcher), *args], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, start_new_session=True, env=env)

        def forward(signum, frame):
            try:
                os.killpg(process.pid, signum)
            except ProcessLookupError:
                pass

        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(signum, forward)
        while chunk := process.stdout.read1(65536):
            log.write(chunk)
            log.flush()
            try:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            except BrokenPipeError:
                pass  # Keep the private log even if the terminal consumer closes.
        code = process.wait()
        result = f'\nRuntime result: {status(code)}; elapsed {time.monotonic() - start:.2f}s\n'
        log.write(result.encode())
    if args == ['--prepare-shaders'] and os.environ.get('ROBLOX_MAC_DATA'):
        # The updater deletes its staging tree. Retain text diagnostics, never AIR/SPIR-V or repro binaries.
        source = Path(os.environ['ROBLOX_MAC_DATA']) / 'metal-build/spv'
        for path in source.glob('*'):
            if path.suffix in ('.out', '.time') and path.is_file():
                (folder / 'shaders').mkdir(exist_ok=True)
                shutil.copyfile(path, folder / 'shaders' / path.name)
    print(result.strip(), file=sys.stderr)
    print(f'Logs saved: {folder}', file=sys.stderr)
    return code if code >= 0 else 128 - code


if __name__ == '__main__':
    sys.exit(main())
