#!/usr/bin/env python3
"""Offline checks for opaque URI transport, lock notifications and the desktop handler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
launcher_source = (ROOT / "bin/roblox-mac").read_text()
bootstrap_source = (ROOT / "package/bootstrap.sh").read_text()
installer_source = (ROOT / "package/install-uri-handler.sh").read_text()
release_builder_source = (ROOT / "package/build-release.sh").read_text()
package_icon = ROOT / "package/icon.png"
release_icon = ROOT.parent.parent / "RobloxLinuxRelease/icon.png"
assert "roblox URI received: scheme=%s length=%s" in launcher_source
assert "pending-uri" not in launcher_source and "retry_pending" not in launcher_source
assert "flock -n -E 73 9" in launcher_source
assert "flock -n -E 73 9" in bootstrap_source
assert "notify-send" in bootstrap_source and "gdbus" in bootstrap_source
assert "ROBLOX_MAC_LAUNCH_URI" in launcher_source
assert 'ICON_SOURCE=$HERE/icon.png' in installer_source
assert 'ICON_PATH=$ICON_DIR/roblox-mac-port.png' in installer_source
assert 'cp "$HERE/icon.png" "$stage/icon.png"' in release_builder_source
assert package_icon.read_bytes() == release_icon.read_bytes()

with tempfile.TemporaryDirectory(prefix="roblox uri ") as temp:
    folder = Path(temp)
    private = folder / "DO_NOT_SHARE"
    private.mkdir(mode=0o700)
    app = private / "missing.app"
    first = 'roblox://place/+%2B:тест "quoted"'
    result = subprocess.run([ROOT / "bin/roblox-mac", "--uri", first],
                            env={**os.environ, "ROBLOX_MAC_CONFIG": "/dev/null",
                                 "ROBLOX_MAC_DATA": str(private), "ROBLOX_MAC_APP": str(app)},
                            capture_output=True, text=True)
    assert first not in result.stdout and first not in result.stderr
    assert "scheme=roblox" in result.stderr and "length=" in result.stderr
    assert not (private / "pending-uri").exists()

    # Exercise the actual outer launcher and --inside re-exec. The former test
    # stopped at the missing-client check and could not catch URI loss there.
    runtime = folder / "runtime"
    (runtime / "bin").mkdir(parents=True)
    (runtime / "scripts").mkdir()
    launcher = runtime / "bin/roblox-mac"
    shutil.copy2(ROOT / "bin/roblox-mac", launcher)
    for name in ("launch-settings.sh", "runtime-limits.py"):
        shutil.copy2(ROOT / "scripts" / name, runtime / "scripts" / name)
    app.mkdir(parents=True)
    (app / "Contents/MacOS").mkdir(parents=True)
    client = app / "Contents/MacOS/RobloxPlayer"
    client.touch(); client.chmod(0o755)
    (private / "prefix").mkdir()
    transport_bin = folder / "transport-bin"
    transport_bin.mkdir()
    fakes = {
        "unshare": """#!/bin/sh
if [ "$1" = -Ur ]; then exit 0; fi
while [ "$#" -gt 0 ]; do
    case "$1" in -*) shift;; *) break;; esac
done
exec "$@"
""",
        "mount": "#!/bin/sh\nexit 0\n",
        "darling": """#!/usr/bin/env python3
import os, shlex, sys
assert sys.argv[1:4] == ['shell', 'bash', '-c']
args = shlex.split(sys.argv[4])
expected = os.environ['TEST_EXPECTED_URI']
assert args.count('-protocolString') == 1
assert os.fsencode(args[args.index('-protocolString') + 1]) == os.fsencode(expected)
assert 'MACOBLOX_PROTOCOL_STRING_PRESENT=1' in args
print('PASS guest command received opaque URI')
""",
    }
    for name, script in fakes.items():
        path = transport_bin / name
        path.write_text(script); path.chmod(0o755)
    for uri in (first, 'roblox-player:opaque+%20%2B:ticket "quotes"',
                os.fsdecode(b'roblox://opaque/%ff\xff')):
        transport_env = {
            **os.environ, "PATH": str(transport_bin) + ":" + os.environ["PATH"],
            "ROBLOX_MAC_ROOT": str(runtime), "ROBLOX_MAC_CONFIG": "/dev/null",
            "ROBLOX_MAC_DATA": str(private), "ROBLOX_MAC_APP": str(app),
            "ROBLOX_MAC_OPTIMIZED": "0", "ROBLOX_MAC_RENDERER": "opengl",
            "ROBLOX_MAC_WAYLAND": "1", "TEST_EXPECTED_URI": uri,
        }
        transport_env.pop("APPDIR", None)
        transport_env.pop("ROBLOX_MAC_LAUNCH_URI", None)
        transport_env.pop("ROBLOX_MAC_LOCK_HELD", None)
        result = subprocess.run([launcher, "--uri", uri], env=transport_env,
                                capture_output=True)
        assert result.returncode == 0, 'outer/inner URI handoff failed'
        assert b'PASS guest command received opaque URI' in result.stdout
        assert os.fsencode(uri) not in result.stdout + result.stderr

    # Prove this check detects the exact regression, rather than just accepting
    # an outer receipt event. The fake Darling never prints the supplied value.
    launcher.write_text(launcher_source.replace('LAUNCH_URI=${ROBLOX_MAC_LAUNCH_URI:-}',
                                               'LAUNCH_URI=', 1))
    broken = subprocess.run([launcher, '--uri', uri], env=transport_env,
                            capture_output=True)
    assert broken.returncode != 0, 'test did not detect namespace URI reset'
    assert os.fsencode(uri) not in broken.stdout + broken.stderr

    release = Path(temp) / "release with spaces"
    release.mkdir()
    shutil.copy2(ROOT / "package/install-uri-handler.sh", release / "install-uri-handler.sh")
    shutil.copy2(package_icon, release / "icon.png")
    (release / "run.sh").write_text("#!/bin/sh\nexit 0\n"); (release / "run.sh").chmod(0o755)
    data = folder / "data"; config = folder / "config"; fakebin = folder / "bin"
    fakebin.mkdir(); calls = folder / "xdg-mime.calls"
    (fakebin / "xdg-mime").write_text("#!/bin/sh\necho \"$*\" >> \"$CALLS\"\n"); (fakebin / "xdg-mime").chmod(0o755)
    env = {**os.environ, "HOME": temp, "XDG_DATA_HOME": str(data), "XDG_CONFIG_HOME": str(config),
           "PATH": str(fakebin) + ":" + os.environ["PATH"], "CALLS": str(calls)}
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True, text=True)
    desktop = data / "applications/roblox-mac-port.desktop"
    subprocess.run(["desktop-file-validate", desktop], check=True)
    text = desktop.read_text()
    assert "NoDisplay" not in text and "%u" in text and "MimeType=x-scheme-handler/roblox;" in text
    assert "Exec=" in text and "release\\ with\\ spaces/run.sh %u" in text
    icon = data / "icons/hicolor/256x256/apps/roblox-mac-port.png"
    assert icon.read_bytes() == (release / "icon.png").read_bytes()
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True, text=True)
    assert len(calls.read_text().splitlines()) == 4
    mime = config / "mimeapps.list"; mime.parent.mkdir()
    mime.write_text("[Default Applications]\nx-scheme-handler/roblox=roblox-mac-port.desktop;other.desktop;\n"
                    "x-scheme-handler/roblox-player=roblox-mac-port.desktop;\n"
                    "x-scheme-handler/other=other.desktop;\n")
    subprocess.run([release / "install-uri-handler.sh", "uninstall"], env=env, check=True, capture_output=True, text=True)
    assert not desktop.exists() and "roblox-mac-port.desktop" not in mime.read_text()
    assert not icon.exists()
    assert "x-scheme-handler/other=other.desktop;" in mime.read_text()
print("PASS opaque URI transport through namespace re-exec and guest command, no pending queue, lock/notification hooks, desktop validation, installer idempotence/uninstall")
