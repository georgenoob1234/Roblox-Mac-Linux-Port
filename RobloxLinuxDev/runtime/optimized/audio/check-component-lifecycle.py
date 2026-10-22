#!/usr/bin/env python3
"""Run the disposed-AudioUnit regression in an isolated guest, without hardware."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile

here = Path(__file__).resolve().parent
runtime = here.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', type=Path, default=here / 'build/component-lifecycle-check')
parser.add_argument('--framework-dir', type=Path,
                    help='Directory containing an alternate AudioToolbox.framework')
args = parser.parse_args()
probe = args.probe.resolve(strict=True)
stage = Path(tempfile.mkdtemp(prefix='audio-lifecycle-check-'))
shutil.copy2(probe, stage / 'component-lifecycle-check')
if args.framework_dir:
    shutil.copytree(args.framework_dir.resolve(strict=True) / 'AudioToolbox.framework',
                    stage / 'AudioToolbox.framework', symlinks=True)
env = {key: value for key, value in os.environ.items()
       if not key.startswith(('DYLD_', 'TRACKA_', 'TRACKB_', 'ROBLOX_MAC_', 'RBX_'))
       and key not in ('APPDIR', 'APPIMAGE', 'LD_PRELOAD')}
env.update(ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA='/mnt/runtime',
           ROBLOX_MAC_WAYLAND='0', ROBLOX_MAC_OPTIMIZED='0', TMPDIR='/mnt')
guest = '/Volumes/SystemRoot/mnt'
command = ['env']
if args.framework_dir:
    command.append('DYLD_FRAMEWORK_PATH=' + guest)
command.append(guest + '/component-lifecycle-check')
launcher = ['unshare', '-Urm', '--propagation', 'private', 'sh', '-c',
            'set -eu; ulimit -c 0; mount --bind "$1" /mnt; exec "$2" --shell "$3"',
            'audio-lifecycle-check', str(stage), str(runtime / 'bin/roblox-mac'), shlex.join(command)]
proc = subprocess.Popen(launcher, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, start_new_session=True)
try:
    output, _ = proc.communicate(timeout=45)
except subprocess.TimeoutExpired:
    os.killpg(proc.pid, signal.SIGKILL)
    output, _ = proc.communicate()
    (stage / 'check.log').write_text(output)
    raise SystemExit(f'Audio lifecycle check timed out; see {stage}/check.log')
(stage / 'check.log').write_text(output)
print(output.strip())
if proc.returncode or output.count('PASS ') != 2:
    raise SystemExit(f'Audio lifecycle check failed (exit {proc.returncode}); see {stage}/check.log')
print(f'PASS isolated AudioToolbox lifecycle regression; logs: {stage}')
