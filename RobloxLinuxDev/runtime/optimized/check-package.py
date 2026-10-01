#!/usr/bin/env python3
"""Reject packages that lose the worker-wakeup fix; check the copied bytes too."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root/'source'
    source.mkdir()
    shutil.copy2(here/'package.py', source/'package.py')
    for name in ('ready', 'libraries.env', 'libtracka-compat.dylib', 'libtracka-video-host.so',
                 'ui/libroblox-wayland.so', 'renderer/libindium.dylib',
                 'renderer/libtracka-present.dylib', 'native/lib/libsystem_malloc.dylib'):
        path = source/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    kernel = source/'native/lib/libsystem_kernel.dylib'
    for label, candidate in (
        ('missing', None),
        ('stock', here.parent/'darling-root/usr/lib/system/libsystem_kernel.dylib'),
        ('fixed', here/'native/lib/libsystem_kernel.dylib'),
    ):
        if candidate:
            shutil.copy2(candidate, kernel)
        destination = root/label
        result = subprocess.run([sys.executable, str(source/'package.py'), str(destination)],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == (label == 'fixed'), (label, result.stderr)
        if label == 'fixed':
            assert (destination/'optimized/native/lib/libsystem_kernel.dylib').read_bytes() == kernel.read_bytes()
        else:
            assert 'worker-wakeup kernel' in result.stderr, result.stderr
            assert not destination.exists(), 'Failed preflight must not copy a partial package'
print('PASS packaging rejects missing/stock kernels and preserves the fixed library')
