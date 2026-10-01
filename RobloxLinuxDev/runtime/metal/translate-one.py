#!/usr/bin/env python3
"""Bound one translation process group and record elapsed seconds / peak RSS KiB."""
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
import time


def group_rss(pid):
    pending = [pid]
    total = 0
    while pending:
        child = pending.pop()
        try:
            status = Path(f'/proc/{child}/status').read_text()
            total += next((int(s.split()[1]) for s in status.splitlines() if s.startswith('VmRSS:')), 0)
            for task in Path(f'/proc/{child}/task').iterdir():
                pending.extend(map(int, (task / 'children').read_text().split()))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return total


def main():
    tool, source, dest = sys.argv[1:]
    # A per-process address-space ceiling backs up the process-group resident-memory check.
    resource.setrlimit(resource.RLIMIT_AS, (500 * 1024**2,) * 2)
    start = time.monotonic()
    peak = 0
    with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as tmp:
        env = dict(os.environ, TMPDIR=tmp)
        if libraries := env.get('ROBLOX_MAC_SHADER_LIBRARY_PATH'):
            env['LD_LIBRARY_PATH'] = libraries
        with open(dest + '.out', 'w') as log:
            process = subprocess.Popen([tool, source, dest + '.spv', '--emit-meta', dest + '.json'],
                                       stdout=log, stderr=log, env=env, start_new_session=True)
            breach = None
            while process.poll() is None:
                peak = max(peak, group_rss(process.pid))
                if time.monotonic() - start >= 20:
                    breach = '20-second translation ceiling exceeded'
                elif peak > 500 * 1024:
                    breach = '500-MiB process-group RSS ceiling exceeded'
                if breach:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    log.write(breach + '\n')
                    break
                time.sleep(0.01)
            code = process.returncode if not breach else 124
    Path(dest + '.time').write_text(f'{time.monotonic() - start:.4f} {peak}\n')
    print('PASS' if code == 0 else 'FAIL')
    return code


if __name__ == '__main__':
    sys.exit(main())
