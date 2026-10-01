#!/usr/bin/env python3
"""Check launch configuration and runtime limits without opening a window."""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
subprocess.run(['sh', '-n', root/'bin/roblox-mac'], check=True)
subprocess.run([sys.executable, root/'probes/check-place-id.py'], check=True)
# Exercise the real guest argument builder without starting Darling or a GUI.
source = (root/'bin/roblox-mac').read_text()
builder = source.split('COMMAND=$(python3 ', 1)[1].split("<<'PYTHON'\n", 1)[1].split('\nPYTHON', 1)[0]
app = '/Volumes/SystemRoot/a client/RobloxPlayer'
link = "roblox-player:1+test:'quoted value';$(echo untouched)"
for supplied, expected in [
    ([], ['-channel', 'production']),
    (['--place', '120189115846709'], ['-channel', 'production', '--place', '120189115846709']),
    (['-channel', 'ztestlinkerset', '--place', '1'], ['-channel', 'ztestlinkerset', '--place', '1']),
    ([link], ['-channel', 'production', link]),
]:
    result = subprocess.check_output([sys.executable, '-c', builder, '/shim with spaces.dylib', app, *supplied],
                                     env={**os.environ, 'ROBLOX_MAC_DATA': '/a data dir'}, text=True)
    arguments = shlex.split(result)
    assert arguments[arguments.index(app):] == [app, *expected]
    assert 'TMPDIR=/Volumes/SystemRoot/a data dir/client-tmp' in arguments[:arguments.index(app)]
    assert 'TEMPDIR=/Volumes/SystemRoot/a data dir/client-tmp' in arguments[:arguments.index(app)]
    assert 'ROBLOX_MAC_PIPELINE_CACHE=/Volumes/SystemRoot/a data dir/vk-pipeline-cache-v1.bin' in arguments[:arguments.index(app)]
command = [sys.executable, root/'scripts/runtime-limits.py', sys.executable, '-c',
           'import os,resource,json;print(json.dumps([resource.getrlimit(resource.RLIMIT_NOFILE),sorted(os.sched_getaffinity(0))]))']
cpu = min(os.sched_getaffinity(0))
result = subprocess.check_output(command, env={**os.environ, 'ROBLOX_MAC_CPU_AFFINITY':str(cpu)}, text=True)
limits, cpus = json.loads(result)
assert 0 < limits[0] <= 65536 and limits[0] == limits[1] and cpus == [cpu]
bad = subprocess.run(command, env={**os.environ, 'ROBLOX_MAC_CPU_AFFINITY':'bad'}, capture_output=True)
assert bad.returncode != 0
with tempfile.TemporaryDirectory(prefix='launch-check-', dir=root/'logs') as tmp:
    path = Path(tmp)/'existing.csv'
    path.write_text('preserve this trace')
    result = subprocess.run([root/'bin/roblox-mac', '--frame-trace', path], capture_output=True)
    assert result.returncode and path.read_text() == 'preserve this trace'
print('PASS: launcher syntax, place IDs, channel defaults/overrides, persistent client temp paths, quoted arguments, CPU affinity, FD limits and trace preservation')
