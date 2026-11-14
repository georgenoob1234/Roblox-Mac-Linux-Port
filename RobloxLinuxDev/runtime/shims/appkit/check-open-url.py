#!/usr/bin/env python3
"""A roblox: link that starts the client must reach it as an open-URL event.

RobloxPlayer joins a place from such a link only when its application delegate
receives application:openURLs: after applicationWillFinishLaunching: and before
applicationDidFinishLaunching: (what macOS does). The AppKit shim delivers the
launcher's link (ROBLOX_MAC_OPEN_URL) from -[NSApplication openFiles]. Two parts:

- open-url-check.m, run in the guest against libappkitgaps: a roblox: link is
  delivered once, at openFiles; nothing is delivered without a link or for a
  link that is not a client link.
- Cocotron's finishLaunching must call openFiles between the two notifications.

The launcher's side (which links travel in ROBLOX_MAC_OPEN_URL, and that
--place-id starts through one) is covered by package/check-uri-handoff.py.
"""
import argparse, os, pathlib, shlex, shutil, subprocess, tempfile

here = pathlib.Path(__file__).resolve().parent
root = here.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library-dir', type=pathlib.Path, default=here, help='directory holding libappkitgaps.dylib')
library = parser.parse_args().library_dir.resolve(strict=True)

source = (root / 'src/darling/src/external/cocotron/AppKit/NSApplication.m').read_text()
launch = source.split('- (void) finishLaunching {', 1)[1].split('\n}\n', 1)[0]
order = [launch.index(mark) for mark in ('postNotificationName: NSApplicationWillFinishLaunchingNotification',
                                         '[self openFiles]',
                                         'postNotificationName: NSApplicationDidFinishLaunchingNotification')]
assert order == sorted(order), 'finishLaunching no longer opens launch documents between its two notifications'

sdk = root / 'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
sysroot = root / 'darling-root'
work = pathlib.Path(tempfile.mkdtemp(prefix='rbx-open-url-', dir='/tmp'))
probe = work / 'open-url-check'
subprocess.run(['clang', '--target=x86_64-apple-macos11', '-O2', '-mtune=generic', '-fuse-ld=lld', '-isysroot', sysroot,
                '-nostdlibinc', '-Wno-objc-root-class', '-Wno-nullability-completeness', '-DTARGET_OS_WASI=0',
                '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1', '-fblocks', '-isystem', sdk / 'usr/include',
                '-F', sdk / 'System/Library/Frameworks', '-F', sysroot / 'System/Library/Frameworks',
                '-framework', 'AppKit', '-framework', 'Foundation', '-L', library, '-lappkitgaps',
                here / 'open-url-check.m', '-o', probe],
               check=True, env=dict(os.environ, TMPDIR=str(work)), stderr=subprocess.DEVNULL)
link = 'roblox://experiences/start?placeId=75634001200249'
cases = [('a roblox: link', link, f'OPEN-URL first {link} second'),
         ('no link', None, 'OPEN-URL first second'),
         ('a web link', 'https://www.roblox.com/games/75634001200249', 'OPEN-URL first second'),
         ('a link with a control character', 'roblox://experiences/start?placeId=1\x07', 'OPEN-URL first second')]
guest = '/Volumes/SystemRoot'
commands = []
for index, (_, value, _) in enumerate(cases):
    variables = ['DYLD_LIBRARY_PATH=' + guest + str(library)] + (['ROBLOX_MAC_OPEN_URL=' + value] if value is not None else [])
    commands.append(shlex.join(['env', '-u', 'ROBLOX_MAC_OPEN_URL', *variables, guest + str(probe)]) + f'; echo "CASE {index} status=$?"')
env = dict(os.environ, ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA=str(work / 'data'), ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'DYLD_INSERT_LIBRARIES', 'DYLD_LIBRARY_PATH', 'ROBLOX_MAC_OPEN_URL'):
    env.pop(name, None)
result = subprocess.run(['timeout', '--kill-after=5s', '90s', str(root / 'bin/roblox-mac'), '--shell', '; '.join(commands)],
                        env=env, capture_output=True, text=True)
lines = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith(('OPEN-URL', 'CASE '))]
expected = [line for index, (_, _, wanted) in enumerate(cases) for line in (wanted, f'CASE {index} status=0')]
assert lines == expected, 'delegate calls differ:\n' + '\n'.join(f'  {name}: wanted "{wanted}"' for name, _, wanted in cases) + \
    '\ngot:\n  ' + '\n  '.join(lines or [(result.stdout + result.stderr)[-600:]])
shutil.rmtree(work, ignore_errors=True)
print('PASS a roblox: link reaches the delegate once when launch documents are opened; other values are not delivered')
