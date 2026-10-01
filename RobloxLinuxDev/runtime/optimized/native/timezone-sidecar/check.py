#!/usr/bin/env python3
"""Check one built libc in a private Darling prefix; never alter the host timezone."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile

here = Path(__file__).resolve().parent
track_a = here.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library', type=Path, required=True)
args = parser.parse_args()
library = args.library.resolve(strict=True)
assert library.name == 'libsystem_c.dylib', library
(track_a / 'logs').mkdir(exist_ok=True)
stage = Path(tempfile.mkdtemp(prefix='timezone-check-', dir=track_a / 'logs'))
(stage / 'lib').mkdir()
content = library.read_bytes()
(stage / 'lib/libsystem_c.dylib').write_bytes(content)
digest = hashlib.sha256(content).hexdigest()
(stage / 'default-zone').write_bytes(Path('/usr/share/zoneinfo/Europe/Paris').read_bytes())

env = {key: value for key, value in os.environ.items()
       if not key.startswith(('DYLD_', 'TRACKA_', 'TRACKB_', 'ROBLOX_MAC_', 'RBX_'))
       and key not in ('APPDIR', 'APPIMAGE', 'LD_PRELOAD', 'TZ')}
env['TMPDIR'] = str(stage)
sdk = track_a / 'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
subprocess.run(['clang', '--target=x86_64-apple-macos11', '-O2', '-fuse-ld=lld',
                '-Wl,--threads=4,-no_adhoc_codesign', '-isysroot', track_a / 'darling-root',
                '-nostdlibinc', '-isystem', sdk / 'usr/include', '-DPLATFORM_MacOSX',
                '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1', here / 'check.c',
                '-o', stage / 'check'], env=env, check=True)
env.update(ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA='/mnt/runtime',
           ROBLOX_MAC_CPU_AFFINITY=','.join(map(str, sorted(os.sched_getaffinity(0))[-4:])),
           ROBLOX_MAC_OPTIMIZED='0', ROBLOX_MAC_RENDERER='opengl', TMPDIR='/mnt')
guest = '/Volumes/SystemRoot/mnt'
# This command runs only after the launcher creates its fresh, private overlay.
command = shlex.join(['ln', '-sf', guest + '/default-zone', '/etc/localtime']) + ' && '
command += shlex.join(['env', 'DYLD_LIBRARY_PATH=' + guest + '/lib',
                      'TZ_CHECK_LIBRARY=' + guest + '/lib/libsystem_c.dylib', guest + '/check'])
launcher = ['unshare', '-Urm', '--propagation', 'private', 'sh', '-c',
            'set -eu; ulimit -c 0; mount --bind "$1" /mnt; exec "$2" --shell "$3"',
            'timezone-check', str(stage), str(track_a / 'bin/roblox-mac'), command]
with subprocess.Popen(launcher, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                      text=True, start_new_session=True) as proc:
    try:
        output, _ = proc.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        output, _ = proc.communicate()
        (stage / 'check.log').write_text(output)
        raise SystemExit(f'Timezone check timed out; see {stage}/check.log')
(stage / 'check.log').write_text(output)
passed = proc.returncode == 0 and output.count('PASS ') == 4
(stage / 'receipt.json').write_text(json.dumps({
    'library': str(library), 'sha256': digest, 'exit_code': proc.returncode, 'passed': passed,
    'copied_library_unchanged': hashlib.sha256((stage / 'lib/libsystem_c.dylib').read_bytes()).hexdigest() == digest,
}, indent=2) + '\n')
assert passed, f'Timezone checks failed; see {stage}/check.log'
print(output.strip())
print(f'PASS libc SHA256 {digest}; logs: {stage}')
