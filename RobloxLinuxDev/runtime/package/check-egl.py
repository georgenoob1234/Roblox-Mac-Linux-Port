#!/usr/bin/env python3
"""Check host graphics drivers with the bundled UI; run on each supported distro."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('appdir', type=Path)
parser.add_argument('--surfaceless', action='store_true', help='Also initialize Mesa EGL without a desktop/GPU')
parser.add_argument('--vulkan', action='store_true', help='Also create a Vulkan instance and enumerate devices (requires vulkaninfo)')
args = parser.parse_args()
root = args.appdir.resolve()
env = dict(os.environ, LD_LIBRARY_PATH=str(root / 'browser/lib'))
helper = root / 'optimized/ui/libroblox-wayland.so'
drivers = []
for directory in ('/etc/glvnd/egl_vendor.d', '/usr/share/glvnd/egl_vendor.d'):
    drivers.extend(json.loads(path.read_text())['ICD']['library_path'] for path in Path(directory).glob('*.json'))
assert drivers, 'No host EGL drivers installed'
for driver in drivers:
    subprocess.run([sys.executable, '-c',
        'import ctypes,sys; ctypes.CDLL(sys.argv[1]); ctypes.CDLL(sys.argv[2]); print("PASS UI + EGL driver:",sys.argv[2])',
        str(helper), driver], env=env, check=True)
if args.surfaceless:
    subprocess.run([sys.executable, '-c', '''
import ctypes as c, sys
c.CDLL(sys.argv[1])
egl = c.CDLL('libEGL.so.1')
egl.eglGetPlatformDisplay.restype = c.c_void_p
egl.eglGetPlatformDisplay.argtypes = [c.c_uint, c.c_void_p, c.c_void_p]
egl.eglInitialize.argtypes = [c.c_void_p, c.POINTER(c.c_int), c.POINTER(c.c_int)]
egl.eglTerminate.argtypes = [c.c_void_p]
display = egl.eglGetPlatformDisplay(0x31DD, None, None)
major, minor = c.c_int(), c.c_int()
assert display and egl.eglInitialize(display, c.byref(major), c.byref(minor)), hex(egl.eglGetError())
print(f'PASS Mesa surfaceless EGL {major.value}.{minor.value}')
assert egl.eglTerminate(display)
''', str(helper)], env=env, check=True)
if args.vulkan:
    # Include every driver's loader errors: software Vulkan succeeding must not
    # hide a hardware driver failing to load because of a bundled dependency.
    result = subprocess.run(['vulkaninfo', '--summary'], env={**env, 'VK_LOADER_DEBUG': 'error,warn'},
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and 'Failed loading library' not in result.stderr, result.stdout + result.stderr
    assert 'deviceName' in result.stdout, result.stdout
    print('PASS Vulkan instance/device enumeration:', '\n'.join(line.strip() for line in result.stdout.splitlines() if 'deviceName' in line))
