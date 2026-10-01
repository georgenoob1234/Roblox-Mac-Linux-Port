#!/usr/bin/env python3
"""Test AudioUnit microphone PCM using only a private PipeWire null sink monitor."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile
import time

here = Path(__file__).resolve().parent
runtime = here.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=here / 'build')
args = parser.parse_args()
build = args.build_dir.resolve(strict=True)
stage = Path(tempfile.mkdtemp(prefix='capture-check-'))
for name in ('CoreAudio.framework', 'CoreAudio.component'):
    shutil.copytree(build / name, stage / name, symlinks=True)
shutil.copy2(here / 'build/capture-check', stage / 'capture-check')
env = {key: value for key, value in os.environ.items()
       if not key.startswith(('DYLD_', 'TRACKA_', 'TRACKB_', 'ROBLOX_MAC_', 'RBX_', 'PIPEWIRE_', 'PULSE_'))
       and key not in ('APPDIR', 'APPIMAGE', 'LD_PRELOAD')}
for directory in ('run', 'config', 'state'):
    (stage / directory).mkdir(mode=0o700)
env.update(XDG_RUNTIME_DIR=str(stage / 'run'), XDG_CONFIG_HOME=str(stage / 'config'),
           XDG_STATE_HOME=str(stage / 'state'), PIPEWIRE_RUNTIME_DIR=str(stage / 'run'),
           PULSE_SERVER='unix:' + str(stage / 'run/pulse/native'),
           ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA='/mnt/runtime',
           ROBLOX_MAC_CPU_AFFINITY=','.join(map(str, sorted(os.sched_getaffinity(0))[-4:])),
           ROBLOX_MAC_OPTIMIZED='0', ROBLOX_MAC_RENDERER='opengl', TMPDIR='/mnt')
processes = []
logs = []
try:
    # The stock PipeWire daemon creates no hardware nodes. WirePlumber's policy
    # profile links our streams but omits every hardware-discovery profile.
    for name, command in (
        ('pipewire', ['pipewire', '-c', '/usr/share/pipewire/pipewire.conf']),
        ('pulse', ['pipewire-pulse', '-c', '/usr/share/pipewire/pipewire-pulse.conf']),
        ('policy', ['wireplumber', '--profile=policy'])):
        log = (stage / (name + '.log')).open('w')
        logs.append(log)
        processes.append(subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                          start_new_session=True))
    for _ in range(100):
        status = subprocess.run(['pactl', 'info'], env=env, capture_output=True)
        if status.returncode == 0:
            break
        time.sleep(0.05)
    assert status.returncode == 0, f'Private audio server failed; see {stage}'
    subprocess.run(['pactl', 'load-module', 'module-null-sink', 'sink_name=tracka_capture',
                    'rate=48000', 'channels=2'], env=env, capture_output=True, check=True)
    sources = subprocess.check_output(['pactl', '--format=json', 'list', 'sources'], env=env, text=True)
    assert [source['name'] for source in json.loads(sources)] == ['tracka_capture.monitor'], sources
    guest = '/Volumes/SystemRoot/mnt'
    command = shlex.join(['env', 'DYLD_FRAMEWORK_PATH=' + guest,
                          'AUDIO_COMPONENT=' + guest + '/CoreAudio.component/Contents/MacOS/CoreAudio',
                          'PULSE_SERVER=' + env['PULSE_SERVER'], guest + '/capture-check'])
    launcher = ['unshare', '-Urm', '--propagation', 'private', 'sh', '-c',
                'set -eu; ulimit -c 0; mount --bind "$1" /mnt; exec "$2" --shell "$3"',
                'capture-check', str(stage), str(runtime / 'bin/roblox-mac'), command]
    proc = subprocess.Popen(launcher, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, start_new_session=True)
    processes.append(proc)
    try:
        output, _ = proc.communicate(timeout=90)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        output, _ = proc.communicate()
        (stage / 'check.log').write_text(output)
        raise SystemExit(f'Capture check timed out; see {stage}/check.log')
    (stage / 'check.log').write_text(output)
    print(output.strip())
    assert proc.returncode == 0 and output.count('PASS ') == 6, f'Capture failed; see {stage}/check.log'
    print(f'PASS private synthetic microphone; logs: {stage}')
finally:
    for proc in reversed(processes):
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    for log in logs:
        log.close()
