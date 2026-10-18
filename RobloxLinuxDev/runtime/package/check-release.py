#!/usr/bin/env python3
"""Offline checks for release paths, flags, update rollback and unsafe downloads."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
build_release = (root / 'package/build-release.sh').read_text()
assert 'installer/installer.py' in build_release and '.roblox-mac-port-version' in build_release
assert 'installer.py' in (Path(__file__).resolve().parents[3] / '.github/scripts/verify-release-archive.sh').read_text()
update_source = (root / 'package/update-roblox.sh').read_text()
assert 'ROBLOX_MAC_PROGRESS_FD' in update_source and 'ROBLOX_MAC_CANCEL_FILE' in update_source
assert 'spv-cache-v1' in update_source and 'previous' in update_source
env = {k: v for k, v in os.environ.items() if not k.startswith(('ROBLOX_', 'APPIMAGE', 'APPDIR', 'TRACKA_NATIVE_MEMORY'))}
with tempfile.TemporaryDirectory(prefix='roblox release ') as tmp:
    folder = Path(tmp)
    release = folder / 'RobloxLinux'
    release.mkdir()
    version = release / 'RobloxVersion'
    version.mkdir()
    flags = release / 'FFlags.json'
    flags.write_text('{"FFlagExample":true}')
    image = release / 'RobloxLinux.AppImage'
    env.update(XDG_DATA_HOME=str(folder / 'private data'), APPDIR=str(root / 'package/AppDir'), APPIMAGE=str(image))
    config = dict(line.split('=', 1) for line in subprocess.check_output(
        [root / 'bin/roblox-mac', '--show-config'], env=env, text=True).splitlines())
    assert config['SWAP_INTERVAL'] == '0'
    assert config['NATIVE_MEMORY'] == '1'
    overridden = dict(line.split('=', 1) for line in subprocess.check_output(
        [root / 'bin/roblox-mac', '--show-config'],
        env={**env, 'ROBLOX_MAC_SWAP_INTERVAL': '1', 'TRACKA_NATIVE_MEMORY': '0'}, text=True).splitlines())
    assert overridden['SWAP_INTERVAL'] == '1' and overridden['NATIVE_MEMORY'] == '0'
    assert config['DATA'] == str(release / 'DO_NOT_SHARE')
    assert config['CONFIG'] == str(release / 'DO_NOT_SHARE/config.env')
    for name in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'TMPDIR'):
        assert Path(config[name]).is_relative_to(release / 'DO_NOT_SHARE'), (name, config[name])
        assert Path(config[name]).is_dir()
    assert not (folder / 'private data').exists()
    assert (release / 'DO_NOT_SHARE').stat().st_mode & 0o777 == 0o700
    rejected = subprocess.run([root / 'bin/roblox-mac', '--show-config'], capture_output=True,
        env={**env, 'ROBLOX_MAC_DATA': str(folder / 'outside')})
    assert rejected.returncode == 2 and not (folder / 'outside').exists()
    # The updater stages beside the client; a release run accepts a data path
    # outside DO_NOT_SHARE only inside the launcher-managed .update-work
    # directory (update-roblox.sh points ROBLOX_MAC_DATA there for the download
    # and for shader preparation) or as a live-launch alias's data directory.
    fakecurl = folder / 'confine-bin'
    fakecurl.mkdir()
    (fakecurl / 'curl').write_text('#!/bin/sh\nexit 1\n')
    (fakecurl / 'curl').chmod(0o755)
    staging = release / '.update-work'
    (staging / 'data').mkdir(parents=True)
    for staged_data in (staging / 'data', staging / 'next' / 'runtime'):
        staged = subprocess.run([root / 'bin/roblox-mac', '--download-client'], capture_output=True,
            env={**env, 'PATH': str(fakecurl) + ':' + env['PATH'],
                 'ROBLOX_MAC_DATA': str(staged_data)})
        assert b'Release data must stay inside DO_NOT_SHARE.' not in staged.stderr, staged.stderr
    alias = os.path.realpath(staging)
    aliased = subprocess.run([root / 'bin/roblox-mac', '--download-client'], capture_output=True,
        env={**env, 'PATH': str(fakecurl) + ':' + env['PATH'],
             'ROBLOX_MAC_ALIAS_DIR': alias, 'ROBLOX_MAC_DATA': alias + '/data'})
    assert b'Release data must stay inside DO_NOT_SHARE.' not in aliased.stderr, aliased.stderr
    stranger = subprocess.run([root / 'bin/roblox-mac', '--download-client'], capture_output=True,
        env={**env, 'PATH': str(fakecurl) + ':' + env['PATH'],
             'ROBLOX_MAC_DATA': str(release / 'stranger-data')})
    assert stranger.returncode == 2 and b'Release data must stay inside DO_NOT_SHARE.' in stranger.stderr
    shutil.rmtree(staging)
    assert config['APP'] == str(version / 'RobloxPlayer.app')
    assert config['FFLAGS'] == str(flags)
    assert config['SHADER_CACHE'] == str(version / 'spv-cache-v1')
    # Synthetic session data only: the updater must preserve the whole profile.
    private = Path(config['DATA'])
    session_files = {
        'cookies/native.sqlite': b'test native session',
        'browser/cookies.sqlite': b'test browser session',
        'browser/data/localstorage/test': b'test browser storage',
        'prefix/Users/root/Library/Preferences/test.plist': b'test client preferences',
    }
    for name, data in session_files.items():
        path = private / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def check_session():
        assert {str(p.relative_to(private)): p.read_bytes() for p in private.rglob('*')
                if p.is_file() and p.name != 'instance.lock'} == session_files
        assert all(p.is_relative_to(private) for p in release.rglob('*.sqlite')), 'Cookies escaped DO_NOT_SHARE'
        assert not (folder / 'private data').exists()

    command = [root / 'scripts/launch-settings.sh', flags, config['APP'], 'true']
    subprocess.run(command, env=env, check=True)
    settings = Path(config['APP']) / 'Contents/MacOS/ClientSettings/ClientAppSettings.json'
    assert json.loads(settings.read_text()) == {'FFlagExample': True}
    previous = settings.read_bytes()
    for invalid in ('{', '[]', '{"Flag": null}'):
        flags.write_text(invalid)
        assert subprocess.run(command, env=env, capture_output=True).returncode
        assert settings.read_bytes() == previous
    flags.write_text('{}')
    subprocess.run(command, env=env, check=True)
    assert json.loads(settings.read_text()) == {}

    shutil.copy2(root / 'package/update-roblox.sh', release)
    shutil.copy2(root / 'package/run.sh', release)
    image.write_text('''#!/bin/sh
set -eu
[ "$APPIMAGELAUNCHER_DISABLE" = 1 ] && [ "$_FORCE_HEADLESS" = 1 ]
[ "$TARGET_APPIMAGE" = "$0" ]
# run.sh must hand every AppImage invocation a mount directory outside the
# release folder, and so outside DO_NOT_SHARE: a FUSE mount below the data
# directory makes the launch-time bind-mount fail with EINVAL.
[ -n "${APPIMAGE_TARGET_DIR:-}" ] && [ -d "$APPIMAGE_TARGET_DIR" ]
case "$APPIMAGE_TARGET_DIR" in "$(dirname -- "$0")"/*) exit 9;; esac
[ "$TMPDIR" = "$(dirname "$0")/DO_NOT_SHARE/tmp" ] && [ -d "$TMPDIR" ]
[ "$1" != --debug ] || shift
case "$1" in
--client-version) echo 'Runtime extraction progress'; echo version-abcd;;
--download-client)
    mkdir -p "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS"
    printf '#!/bin/sh\\n' > "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    chmod +x "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    echo version-abcd > "$ROBLOX_MAC_DATA/.version";;
--prepare-shaders)
    [ "${FAIL_PREPARE:-0}" != missing-cache ] || exit 0
    [ "${FAIL_PREPARE:-0}" = 0 ] || exit 1
    mkdir -p "$(dirname "$ROBLOX_MAC_APP")/spv-cache-v1"
    echo '{}' > "$(dirname "$ROBLOX_MAC_APP")/spv-cache-v1/report.json";;
*) exit 2;;
esac
''')
    image.chmod(0o755)
    (version / '.version').write_text('version-old')
    update = ['sh', release / 'update-roblox.sh']
    for failure in ('1', 'missing-cache'):
        result = subprocess.run(update, env={**env, 'FAIL_PREPARE': failure}, capture_output=True)
        assert result.returncode and (version / '.version').read_text() == 'version-old'
        assert b'Shader preparation failed' in result.stderr and b'mv:' not in result.stderr
        assert settings.read_bytes() == b'{}'
        assert not list(private.glob('.update.*'))
        check_session()
    subprocess.run(update, env=env, check=True)
    assert (version / '.version').read_text().strip() == 'version-abcd'
    assert flags.read_text() == '{}'
    assert not list(private.glob('.update.*'))
    check_session()
    assert 'already up to date' in subprocess.check_output(update, env=env, text=True)
    check_session()
    with open(Path(config['DATA']) / 'instance.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        assert subprocess.run(update, env=env, capture_output=True).returncode

    # Real downloader: reject ZIP traversal before touching the previous client.
    fakebin = folder / 'bin'
    fakebin.mkdir()
    archive = folder / 'bad.zip'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('../escaped', 'bad')
    curl = fakebin / 'curl'
    curl.write_text('''#!/bin/sh
case "$*" in
*client-version*) echo '{"clientVersionUpload":"version-abcd"}';;
*) cp "$TEST_ARCHIVE" "$3";;
esac
''')
    curl.chmod(0o755)
    original = version / 'RobloxPlayer.app/Contents/MacOS/RobloxPlayer'
    before = original.read_bytes()
    (version / '.version').write_text('version-old')
    result = subprocess.run([root / 'scripts/fetch-client.sh'], capture_output=True,
        env={**env, 'PATH': str(fakebin) + ':' + env['PATH'], 'TEST_ARCHIVE': str(archive), 'ROBLOX_MAC_DATA': str(version)})
    assert result.returncode and b'Unsafe client archive' in result.stderr
    assert original.read_bytes() == before and not (folder / 'escaped').exists()

    # Real downloader: a CDN connection reset must be retried, not fatal.
    good = folder / 'good.zip'
    exe = zipfile.ZipInfo('RobloxPlayer.app/Contents/MacOS/RobloxPlayer')
    exe.create_system = 3
    exe.external_attr = 0o100755 << 16
    with zipfile.ZipFile(good, 'w') as z:
        z.writestr(exe, b'#!/bin/sh\n')
    state = folder / 'curl-state'
    curl.write_text('''#!/bin/sh
case "$1" in *I*) exit 0;; esac
case "$*" in
*client-version*) echo '{"clientVersionUpload":"version-abcd"}';;
*)
    n=$(cat "$TEST_STATE" 2>/dev/null || echo 0)
    n=$((n + 1))
    printf '%s\\n' "$n" > "$TEST_STATE"
    if [ "$n" -lt 3 ]; then
        echo 'curl: (35) Recv failure: Connection reset by peer' >&2
        exit 35
    fi
    cp "$TEST_ARCHIVE" "$3"
    ;;
esac
''')
    (version / '.version').write_text('version-old')
    result = subprocess.run([root / 'scripts/fetch-client.sh'], capture_output=True,
        env={**env, 'PATH': str(fakebin) + ':' + env['PATH'], 'TEST_ARCHIVE': str(good),
             'TEST_STATE': str(state), 'ROBLOX_MAC_DATA': str(version)})
    assert result.returncode == 0, result.stderr
    assert (version / '.version').read_text() == 'version-abcd'
    assert state.read_text().strip() == '3', state.read_text()
    assert (version / 'RobloxPlayer.app/Contents/MacOS/RobloxPlayer').exists()

    pack = version / 'RobloxPlayer.app/Contents/Resources/shaders/shaders_metal_osx.pack'
    pack.parent.mkdir(parents=True)
    pack.write_bytes(b'')
    result = subprocess.run([sys.executable, root / 'metal/prepare-cache.py', '--app', pack.parents[3],
        '--source', folder / 'missing-shaders', '--output', folder / 'empty-cache'], capture_output=True)
    assert result.returncode and not (folder / 'empty-cache/report.json').exists()
print('PASS release paths, FFlags validation/reset, update rollback/success/no-op/lock, session preservation, ZIP traversal rejection')
