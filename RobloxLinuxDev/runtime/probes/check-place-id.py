#!/usr/bin/env python3
"""Reject malformed direct-join input before launching a GUI or namespace."""
from pathlib import Path
import subprocess

launcher = Path(__file__).resolve().parents[1] / 'bin/roblox-mac'
for args in [[], [''], ['0'], ['000'], ['-1'], ['1.5'], ['123;echo injected'],
             ['999999999999999999999999999'], ['2248408710', 'extra']]:
    result = subprocess.run([launcher, '--place-id', *args], capture_output=True, text=True)
    assert result.returncode == 2 and 'Usage:' in result.stderr, (args, result.returncode)
print('PASS invalid place IDs fail before launching Roblox')
