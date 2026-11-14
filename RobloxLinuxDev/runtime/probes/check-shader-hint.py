#!/usr/bin/env python3
"""The hint to prepare the shader cache must name a command that still exists.

Inside an AppImage $0 is a path in the temporary mount, which is gone when the
message is read; the hint has to name the AppImage file instead.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
launcher = root / 'bin/roblox-mac'
with tempfile.TemporaryDirectory(prefix='shader-hint-') as temporary:
    stage = Path(temporary)
    binary = stage / 'client/RobloxPlayer.app/Contents/MacOS/RobloxPlayer'
    binary.parent.mkdir(parents=True)
    binary.write_text('#!/bin/sh\nexit 1\n')
    binary.chmod(0o755)
    appimage = stage / 'Roblox-Port.AppImage'
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('ROBLOX_MAC_', 'TRACKA_', 'TRACKB_', 'RBX_')) and key not in ('APPDIR', 'APPIMAGE')}
    env.update(ROBLOX_MAC_CONFIG='/dev/null', ROBLOX_MAC_DATA=str(stage / 'data'), ROBLOX_MAC_RENDERER='metal',
               ROBLOX_MAC_APP=str(binary.parents[2]))
    for extra, command in (({'APPDIR': str(root), 'APPIMAGE': str(appimage)}, str(appimage)),
                           ({'APPDIR': str(root)}, str(launcher)),      # an unpacked AppDir
                           ({'APPIMAGE': str(appimage)}, str(launcher)),  # started from some other AppImage
                           ({}, str(launcher))):
        result = subprocess.run([launcher], env={**env, **extra}, capture_output=True, text=True, timeout=60)
        wanted = f'roblox-mac: prepare the Metal shader cache first: {command} --prepare-shaders\n'
        assert result.returncode == 1 and result.stderr.endswith(wanted), (extra, result.returncode, result.stderr[-400:])
print('PASS the shader-cache hint names the AppImage, or the launcher outside one')
