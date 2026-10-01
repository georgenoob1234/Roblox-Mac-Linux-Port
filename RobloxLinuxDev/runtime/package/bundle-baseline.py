#!/usr/bin/env python3
"""Runs only in baseline.Dockerfile's build environment; writes the staged AppDir."""
import os
from pathlib import Path
import re
import shutil
import subprocess

source, work, app = Path('/src'), Path('/tmp/build'), Path('/out')
runtime = work / 'runtime'
runtime.mkdir(parents=True)
for name in ('browser', 'profiler'):
    shutil.copytree(source / 'runtime' / name, runtime / name,
                    ignore=shutil.ignore_patterns('build', 'lib', 'protocols', '*.o', '*.a', '*.so', '*.bin'))
(runtime / 'src').mkdir()
(runtime / 'src/Vulkan-Headers-1.3.290').symlink_to(source / 'runtime/src/Vulkan-Headers-1.3.290')
(runtime / 'logs').mkdir()
(runtime / 'optimized').mkdir()
shutil.copy2(source / 'runtime/optimized/ui-build.sh', runtime / 'optimized')
(work / 'native').mkdir()
shutil.copy2(source / 'native/wayland.cpp', work / 'native')
env = dict(os.environ, ROBLOX_MAC_PORTABLE_ROOT='/')
for script in ('browser/build.sh', 'optimized/ui-build.sh'):
    subprocess.run(['sh', str(runtime / script)], env=env, check=True)
dest = app / 'browser'
if dest.exists():
    shutil.rmtree(dest)
(dest / 'lib').mkdir(parents=True)
for name in ('roblox-gtk', 'roblox-gtk.bin', 'libroblox-wayland.so'):
    shutil.copy2(runtime / 'browser' / name, dest / name)
shutil.copy2(runtime / 'optimized/ui/libroblox-wayland.so', app / 'optimized/ui')
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libavcodec', 'libavutil', 'libswscale'], text=True).split()
subprocess.run(['cc', '-O2', '-march=x86-64', '-mtune=generic', '-shared', '-fPIC',
    str(source / 'runtime/optimized/video-host.c'), '-o', str(app / 'optimized/libtracka-video-host.so'),
    *flags, '-Wl,--disable-new-dtags,-rpath,$ORIGIN/../browser/lib'], check=True)

system = Path('/usr/lib/x86_64-linux-gnu')
# The shader translator needs newer LLVM than Ubuntu ships. Keep only its
# missing libraries and newer C++ ABI; all are checked against glibc 2.39 below.
fallback = Path('/tmp/shader-libs')
fallback.mkdir()
# Host EGL/Vulkan drivers need matching Wayland and XCB display libraries.
skip = re.compile(r'^(ld-linux|lib(c|m|mvec|pthread|dl|rt|resolv|nss_.*|util|GL|GLX|GLdispatch|GLES.*|EGL|OpenGL|vulkan|nvidia.*|drm.*|gbm|wayland-.*|xcb(?:-.*)?)\.so)')
for path in Path('/portable/usr/lib').glob('*.so*'):
    if skip.match(path.name) or not path.is_file():
        continue
    if (system / path.name).exists() and not path.name.startswith(('libstdc++.', 'libgcc_s.')):
        continue
    shutil.copy2(path, fallback / path.name)
for name in ('llvm-dis', 'spirv-val'):
    shutil.copy2(Path('/portable/usr/bin') / name, app / 'usr/bin' / name)

webkit = system / 'webkit2gtk-4.1'
shutil.copytree(webkit, dest / 'lib/webkit2gtk-4.1',
                ignore=shutil.ignore_patterns('MiniBrowser', 'jsc'))
shutil.copytree(system / 'gio/modules', dest / 'lib/gio/modules')
pixbuf = system / 'gdk-pixbuf-2.0/2.10.0'
shutil.copytree(pixbuf, dest / 'lib/gdk-pixbuf-2.0')
cache = dest / 'lib/gdk-pixbuf-2.0/loaders.cache'
cache.write_text(cache.read_text().replace(str(pixbuf), './lib/gdk-pixbuf-2.0'))
shutil.copytree('/usr/share/glib-2.0/schemas', dest / 'share/glib-2.0/schemas')
shutil.copytree('/etc/fonts', dest / 'share/fontconfig')
gst = dest / 'lib/gstreamer-1.0'
gst.mkdir()
plugins = set()
for package in ('libgstreamer1.0-0', 'gstreamer1.0-plugins-base', 'gstreamer1.0-pipewire', 'gstreamer1.0-libav'):
    for name in subprocess.check_output(['dpkg-query', '-L', package], text=True).splitlines():
        path = Path(name)
        if path.parent == system / 'gstreamer-1.0' and path.suffix == '.so':
            plugins.add(path)
for name in ('videoparsersbad', 'autodetect', 'pulseaudio', 'wavparse', 'audioparsers', 'isomp4', 'matroska', 'soup'):
    plugins.add(system / 'gstreamer-1.0' / f'libgst{name}.so')
for plugin in plugins:
    shutil.copy2(plugin, gst / plugin.name)
shutil.copy2(system / 'gstreamer1.0/gstreamer-1.0/gst-plugin-scanner', gst / 'gst-plugin-scanner')

def elf(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as file:
        return file.read(4) == b'\x7fELF'

# Mach-O adapters dlopen Linux libraries by soname, so ldd cannot discover them.
wrappers = list((app / 'darling-root/usr/lib/native').glob('*.dylib'))
wrappers += list((app / 'darling-root/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries').glob('*.dylib'))
for wrapper in wrappers:
    for name in set(re.findall(rb'lib[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*', wrapper.read_bytes())):
        name = name.decode()
        if not skip.match(name):
            shutil.copy2(system / name, dest / 'lib' / name)
queue = [path for path in app.rglob('*') if elf(path)]
seen = set()
environment = dict(os.environ, LD_LIBRARY_PATH=str(fallback))
while queue:
    path = queue.pop()
    if path in seen:
        continue
    seen.add(path)
    result = subprocess.run(['ldd', str(path)], env=environment, text=True, capture_output=True)
    output = result.stdout + result.stderr
    if 'not found' in output or (result.returncode and 'not a dynamic executable' not in output and 'statically linked' not in output):
        raise SystemExit(f'Baseline dependencies failed for {path}:\n{output}')
    for name, resolved in re.findall(r'^\s*(\S+) => (/\S+) ', output, re.M):
        if '/' in name or skip.match(name):
            continue
        target = dest / 'lib' / name
        if target.exists():
            continue
        shutil.copy2(resolved, target)
        queue.append(Path(resolved))

# WebKit's production build embeds an absolute subprocess path. Keep the
# existing relative-to-browser convention and preserve the binary's length.
lib = dest / 'lib/libwebkit2gtk-4.1.so.0'
data = lib.read_bytes()
old = str(webkit).encode()
tail = b'lib/webkit2gtk-4.1'
new = b'.' + b'/' * (len(old) - len(tail) - 1) + tail
assert old in data and len(old) == len(new)
lib.write_bytes(data.replace(old, new))
licenses = dest / 'share/licenses'
licenses.mkdir()
shutil.copytree('/usr/share/licenses', licenses, dirs_exist_ok=True)
shutil.copytree('/portable/usr/share/licenses', licenses / 'shader-packages', dirs_exist_ok=True)
for copyright in Path('/usr/share/doc').glob('*/copyright'):
    shutil.copy2(copyright, licenses / (copyright.parent.name + '.copyright'))
packages = subprocess.check_output(['dpkg-query', '-W', '-f=${Package} ${Version} ${Architecture}\n'], text=True)
(dest / 'BUILD-PACKAGES.txt').write_text(packages + 'SDL3 3.4.2 source; libedit 20260512-3.1 source (SHA-256 in baseline.Dockerfile)\n')
shutil.copy2('/portable/packages.json', dest / 'SHADER-PACKAGES.json')
subprocess.run(['python3', str(source / 'runtime/package/check-portable.py'), '--appdir', str(app)], check=True)
subprocess.run(['python3', str(source / 'runtime/package/check-egl.py'), str(app), '--surfaceless', '--vulkan'], check=True)
environment['LD_LIBRARY_PATH'] = str(dest / 'lib')
subprocess.run(['python3', '-c', 'import ctypes; ctypes.CDLL("/out/optimized/ui/libroblox-wayland.so"); print("PASS native UI load on glibc 2.39")'], env=environment, check=True)
for tool in ('llvm-dis', 'spirv-val'):
    subprocess.run([str(app / 'usr/bin' / tool), '--version'], env=environment, check=True)
print('Bundled Ubuntu 24.04 UI/media libraries and verified native loading on glibc 2.39.')
