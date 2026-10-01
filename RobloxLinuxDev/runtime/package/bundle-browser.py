#!/usr/bin/env python3
"""Build and bundle the native UI against Ubuntu 24.04, never the rolling host."""
import os
from pathlib import Path
import subprocess
import sys
from portable_runtime import prepare

root = Path(__file__).resolve().parents[1]
prefix = prepare(root / 'package/deps/portable')
app = Path(sys.argv[1]).resolve()
with (root / 'package/baseline.Dockerfile').open('rb') as recipe:
    subprocess.run(['docker', 'build', '-t', 'roblox-baseline:24.04', '-'], stdin=recipe, check=True)
subprocess.run(['docker', 'run', '--rm', '--network=none', '--user', f'{os.getuid()}:{os.getgid()}',
    '-v', f'{root.parent}:/src:ro', '-v', f'{app}:/out', '-v', f'{prefix}:/portable:ro',
    'roblox-baseline:24.04', 'python3', '/src/runtime/package/bundle-baseline.py'], check=True)
