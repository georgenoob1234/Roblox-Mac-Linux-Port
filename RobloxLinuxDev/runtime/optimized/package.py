#!/usr/bin/env python3
"""Copy only the native runtime artifacts into the AppImage, preserving relative paths."""
from pathlib import Path
import hashlib
import shutil
import sys

here = Path(__file__).resolve().parent
destination = Path(sys.argv[1]).resolve() / 'optimized'
files = ['libraries.env', 'libtracka-compat.dylib', 'libtracka-video-host.so', 'ui/libroblox-wayland.so',
         'renderer/libindium.dylib', 'renderer/libtracka-present.dylib']
files += [str(p.relative_to(here)) for p in sorted((here/'native/lib').glob('*.dylib'))]
if not (here/'ready').is_file() or not (here/'native/lib/libsystem_malloc.dylib').is_file():
    raise SystemExit('Build and validate the optimized native libraries first')
kernel = here/'native/lib/libsystem_kernel.dylib'
if not kernel.is_file() or hashlib.sha256(kernel.read_bytes()).hexdigest() != '76946f15d3722f886640a96811f9aaae852881320f2214c7c9fdb3969f647548':
    raise SystemExit('Build the validated worker-wakeup kernel with optimized/native/build.py first')
for name in files:
    source = here/name
    target = destination/name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
frameworks = here/'native/frameworks'
if frameworks.is_dir():
    shutil.copytree(frameworks, destination/'native/frameworks', symlinks=True, dirs_exist_ok=True)
shutil.copy2(here/'ready', destination/'ready')
