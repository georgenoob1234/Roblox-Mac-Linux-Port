#!/usr/bin/env python3
"""Drive device-check --synthetic against a private PipeWire server.

Covers the host device inventory: enumeration, a default-output change
notification, and a sink that is added, removed and added again while the
client is running. Only private null sinks are used; no hardware is touched.

The sink comes back after a restart of the sound server, so the check also
proves that a restart neither kills the client nor leaves the inventory
stale, and that streams running across it play and record again. The probe
therefore runs on the library set the game is launched
with: Darling's stock libSystem lacks the deferred knote delete of
native/runtime-sidecar/kqueue-delete, and without it libdispatch crashes
(_dispatch_kevent_merge on a freed source) when the server goes away.
"""
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
stage = Path(tempfile.mkdtemp(prefix='device-inventory-check-'))
for name in ('CoreAudio.framework', 'CoreAudio.component'):
    shutil.copytree(build / name, stage / name, symlinks=True)
shutil.copy2(build / 'device-check', stage / 'device-check')
env = {key: value for key, value in os.environ.items()
       if not key.startswith(('DYLD_', 'TRACKA_', 'TRACKB_', 'ROBLOX_MAC_', 'RBX_', 'PIPEWIRE_', 'PULSE_'))
       and key not in ('APPDIR', 'APPIMAGE', 'LD_PRELOAD')}
for directory in ('run', 'config', 'state', 'handshake'):
    (stage / directory).mkdir(mode=0o700)
env.update(XDG_RUNTIME_DIR=str(stage / 'run'), XDG_CONFIG_HOME=str(stage / 'config'),
           XDG_STATE_HOME=str(stage / 'state'), PIPEWIRE_RUNTIME_DIR=str(stage / 'run'),
           PULSE_SERVER='unix:' + str(stage / 'run/pulse/native'),
           ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA='/mnt/runtime',
           ROBLOX_MAC_CPU_AFFINITY=','.join(map(str, sorted(os.sched_getaffinity(0))[-4:])),
           ROBLOX_MAC_OPTIMIZED='0', ROBLOX_MAC_RENDERER='opengl', TMPDIR='/mnt')
handshake = stage / 'handshake'
processes = []
logs = []


def pactl(*arguments):
    return subprocess.run(['pactl', *arguments], env=env, capture_output=True, text=True, check=True).stdout


def null_sink(name):
    return pactl('load-module', 'module-null-sink', 'sink_name=' + name, 'rate=48000', 'channels=2').strip()


def inventory_clients():
    """Server-side ids of the CoreAudio inventory connections that exist right now."""
    clients = json.loads(pactl('--format=json', 'list', 'clients'))
    return [client['index'] for client in clients
            if client.get('properties', {}).get('application.name') == 'CoreAudio device inventory']


def step(proc, ready, checked, action=None, timeout=40):
    """Wait for the probe's `ready` file, run `action`, then release the probe."""
    deadline = time.monotonic() + timeout
    while not (handshake / ready).exists():
        if proc.poll() is not None:
            raise SystemExit(f'device-check exited before {ready}; see {stage}/check.log')
        if time.monotonic() > deadline:
            raise SystemExit(f'device-check never reached {ready}; see {stage}/check.log')
        time.sleep(0.05)
    if action:
        action()
    (handshake / checked).write_text('ready')


def start(name, command):
    log = (stage / (name + '.log')).open('a')
    logs.append(log)
    proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    processes.append(proc)
    return proc


def stop(proc):
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def start_pulse():
    proc = start('pulse', ['pipewire-pulse', '-c', '/usr/share/pipewire/pipewire-pulse.conf'])
    for _ in range(100):
        status = subprocess.run(['pactl', 'info'], env=env, capture_output=True)
        if status.returncode == 0:
            break
        time.sleep(0.05)
    assert status.returncode == 0, f'Private audio server failed; see {stage}'
    return proc


try:
    start('pipewire', ['pipewire', '-c', '/usr/share/pipewire/pipewire.conf'])
    start('policy', ['wireplumber', '--profile=policy'])
    pulse = start_pulse()
    null_sink('tracka_test_A')
    null_sink('tracka_test_B')
    pactl('set-default-sink', 'tracka_test_A')
    guest = '/Volumes/SystemRoot/mnt'
    native = '/Volumes/SystemRoot' + str(runtime / 'optimized/native/lib')
    command = shlex.join(['env', 'DYLD_FORCE_FLAT_NAMESPACE=1', 'DYLD_LIBRARY_PATH=' + native,
                          'DYLD_INSERT_LIBRARIES=' + native + '/libtracka-clock.dylib:' + native + '/librbxkqueue.dylib',
                          'DYLD_FRAMEWORK_PATH=' + guest,
                          'AUDIO_COMPONENT=' + guest + '/CoreAudio.component/Contents/MacOS/CoreAudio',
                          'PULSE_SERVER=' + env['PULSE_SERVER'],
                          guest + '/device-check', '--synthetic', guest + '/handshake'])
    launcher = ['unshare', '-Urm', '--propagation', 'private', 'sh', '-c',
                'set -eu; ulimit -c 0; mount --bind "$1" /mnt; exec "$2" --shell "$3"',
                'device-inventory-check', str(stage), str(runtime / 'bin/roblox-mac'), command]
    check_log = (stage / 'check.log').open('w')
    logs.append(check_log)
    proc = subprocess.Popen(launcher, env=env, stdout=check_log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    processes.append(proc)
    module = {}
    seen = []

    def restart_server():
        """Stop the sound server under the running probe and bring sink C back on a new one."""
        global pulse
        seen.append(inventory_clients())
        stop(pulse)
        processes.remove(pulse)
        pulse = start_pulse()
        for name in ('tracka_test_A', 'tracka_test_B', 'tracka_test_C'):
            null_sink(name)
        pactl('set-default-sink', 'tracka_test_B')
        deadline = time.monotonic() + 10
        while not inventory_clients():
            assert time.monotonic() < deadline, 'Inventory did not reconnect after the server restart'
            time.sleep(0.1)

    # The probe is playing into B and recording its monitor. Moving the default
    # sink now must reach its default-output listener through a change event.
    step(proc, 'routing-ready', 'routing-checked', lambda: pactl('set-default-sink', 'tracka_test_B'))
    step(proc, 'unit-input-ready', 'unit-input-checked', lambda: time.sleep(0.5))
    step(proc, 'unit-default-ready', 'unit-default-checked', lambda: time.sleep(0.5))
    step(proc, 'hotplug-ready', 'hotplug-added',
         lambda: (seen.append(inventory_clients()), module.update(c=null_sink('tracka_test_C'))))
    step(proc, 'hotplug-seen', 'hotplug-removed', lambda: pactl('unload-module', module['c']))
    step(proc, 'hotplug-gone', 'hotplug-restored', restart_server)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        raise SystemExit(f'Device inventory check timed out; see {stage}/check.log')
    check_log.flush()
    output = (stage / 'check.log').read_text()
    print('\n'.join(line for line in output.splitlines() if line.startswith(('PASS ', 'AUDIO SIGNAL', 'Assertion'))))
    assert proc.returncode == 0 and output.count('PASS ') == 8, f'Device inventory failed; see {stage}/check.log'
    # The inventory must be one long-lived connection that learns of changes
    # through events. Reconnecting for every poll (the old behaviour, ~23 ms of
    # stalled CoreAudio calls per second) gives a new client id each time.
    assert len(seen[0]) == 1 and seen[0] == seen[1], f'Inventory connection was not kept open: {seen}'
    print(f'PASS private device inventory, default change, hotplug and sound-server restart; logs: {stage}')
finally:
    for proc in reversed(processes):
        if proc.poll() is None:
            stop(proc)
    for log in logs:
        log.close()
