#!/usr/bin/env python3
"""Regression test for Linux taskbar icons, window identity, and desktop entry matching."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

PACKAGE = Path(__file__).resolve().parent
REPO = PACKAGE.parents[2]
BROWSER = REPO / "RobloxLinuxDev/runtime/browser"
APPIMAGE_SCRIPT = PACKAGE / "build-appimage.sh"
INSTALL_SCRIPT = PACKAGE / "install-uri-handler.sh"
RUN_SCRIPT = PACKAGE / "run.sh"
UI_SOURCE = PACKAGE / "bootstrapper-ui.c"
HOST_SOURCE = BROWSER / "host.cpp"
WAYLAND_SOURCE = BROWSER / "wayland.cpp"

def test_no_synthetic_red_box():
    text = APPIMAGE_SCRIPT.read_text()
    assert "#E2231A" not in text, "Found synthetic red box color #E2231A in build-appimage.sh"
    assert "226, 35, 26" not in text, "Found synthetic red box RGB in build-appimage.sh"
    assert "zlib.compress" not in text, "Found inline PNG generator in build-appimage.sh"
    print("PASS no synthetic red square generator in build-appimage.sh")

def test_appimage_icon_setup():
    text = APPIMAGE_SCRIPT.read_text()
    assert 'Icon=roblox-mac-port' in text, "Missing Icon=roblox-mac-port in build-appimage.sh desktop entry"
    assert 'StartupWMClass=roblox-mac-port' in text, "Missing StartupWMClass=roblox-mac-port in build-appimage.sh desktop entry"
    assert 'cp "$HERE/icon.png" "$APPDIR/roblox-mac-port.png"' in text, "Missing roblox-mac-port.png copy in build-appimage.sh"
    assert 'cp "$HERE/icon.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/roblox-mac-port.png"' in text, "Missing hicolor app icon copy in build-appimage.sh"
    assert 'cp "$HERE/icon_bw.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/roblox-mac-port-settings.png"' in text, "Missing settings icon copy in build-appimage.sh"
    assert 'ln -sf roblox-mac-port.png "$APPDIR/.DirIcon"' in text, "Missing .DirIcon symlink in build-appimage.sh"
    print("PASS AppImage icon bundle configuration")

def test_desktop_entries():
    with tempfile.TemporaryDirectory(prefix="roblox-identity-") as tmpdir:
        temp = Path(tmpdir)
        data = temp / "data"
        config = temp / "config"
        fake_bin = temp / "bin"
        fake_bin.mkdir()
        (fake_bin / "xdg-mime").write_text("#!/bin/sh\nexit 0\n")
        (fake_bin / "xdg-mime").chmod(0o755)

        env = {
            **os.environ,
            "HOME": str(temp),
            "XDG_DATA_HOME": str(data),
            "XDG_CONFIG_HOME": str(config),
            "PATH": str(fake_bin) + ":" + os.environ.get("PATH", ""),
        }
        res = subprocess.run([INSTALL_SCRIPT, "install"], env=env, capture_output=True, text=True)
        assert res.returncode == 0, f"install-uri-handler.sh failed: {res.stderr}"

        main_desktop = data / "applications/roblox-mac-port.desktop"
        assert main_desktop.exists(), "Main desktop file was not created"
        main_text = main_desktop.read_text()
        assert "Icon=roblox-mac-port" in main_text, "Missing Icon=roblox-mac-port"
        assert "StartupWMClass=roblox-mac-port" in main_text, "Missing StartupWMClass=roblox-mac-port in main desktop"

        settings_desktop = data / "applications/roblox-mac-port-settings.desktop"
        assert settings_desktop.exists(), "Settings desktop file was not created"
        settings_text = settings_desktop.read_text()
        assert "Icon=roblox-mac-port-settings" in settings_text, "Missing Icon=roblox-mac-port-settings"
        assert "StartupWMClass=roblox-mac-port" in settings_text, "Missing StartupWMClass=roblox-mac-port in settings desktop"

        main_icon = data / "icons/hicolor/256x256/apps/roblox-mac-port.png"
        assert main_icon.exists(), "Main icon was not installed"
        assert main_icon.stat().st_size > 10000, "Main icon file is unexpectedly small"

        settings_icon = data / "icons/hicolor/256x256/apps/roblox-mac-port-settings.png"
        assert settings_icon.exists(), "Settings icon was not installed"
        assert settings_icon.stat().st_size > 10000, "Settings icon file is unexpectedly small"

        if subprocess.run(["which", "desktop-file-validate"], capture_output=True).returncode == 0:
            subprocess.run(["desktop-file-validate", str(main_desktop)], check=True)
            subprocess.run(["desktop-file-validate", str(settings_desktop)], check=True)

    print("PASS desktop entry and icon installation")

def test_run_script_icon_propagation():
    text = RUN_SCRIPT.read_text()
    assert 'export ROBLOX_MAC_ICON_PATH=' in text, "Missing ROBLOX_MAC_ICON_PATH export in run.sh"
    print("PASS run.sh icon environment propagation")

def test_browser_host_and_wayland_identity():
    host_text = HOST_SOURCE.read_text()
    assert 'g_set_prgname("roblox-mac-port")' in host_text, "Missing g_set_prgname(\"roblox-mac-port\") in host.cpp"
    assert 'g_set_prgname("roblox-mac")' not in host_text, "Old g_set_prgname(\"roblox-mac\") still present in host.cpp"
    assert 'gdk_set_program_class("roblox-mac-port")' in host_text, "Missing gdk_set_program_class(\"roblox-mac-port\") in host.cpp"
    assert 'gtk_window_set_default_icon_name("roblox-mac-port")' in host_text, "Missing default icon name in host.cpp"
    assert 'gtk_window_set_icon_name(GTK_WINDOW(win), "roblox-mac-port")' in host_text, "Missing window icon name in host.cpp"
    assert 'setup_window_identity(window)' in host_text, "Missing setup_window_identity call in host.cpp"

    wayland_text = WAYLAND_SOURCE.read_text()
    assert 'name[]="roblox-mac-port"' in wayland_text, "Missing name[]=\"roblox-mac-port\" in wayland.cpp"
    assert 'name[]="roblox-mac"' not in wayland_text, "Old name[]=\"roblox-mac\" still present in wayland.cpp"
    print("PASS browser host and wayland identity")

def test_bootstrapper_ui_identity():
    ui_text = UI_SOURCE.read_text()
    assert 'g_set_prgname("roblox-mac-port")' in ui_text, "Missing g_set_prgname(\"roblox-mac-port\") in bootstrapper-ui.c"
    assert 'gdk_set_program_class("roblox-mac-port")' in ui_text, "Missing gdk_set_program_class in bootstrapper-ui.c"
    assert 'gtk_window_set_default_icon_name("roblox-mac-port")' in ui_text, "Missing default icon name in bootstrapper-ui.c"
    assert 'setup_ui_identity' in ui_text, "Missing setup_ui_identity in bootstrapper-ui.c"
    assert 'setup_ui_identity(state->window, state->root)' in ui_text, "Missing setup_ui_identity call in progress or launcher"
    assert 'setup_ui_identity(state.window, NULL)' in ui_text, "Missing setup_ui_identity call in dialog"
    print("PASS bootstrapper-ui window identity and icon setup")

def test_compiled_binaries():
    ui_bin = PACKAGE / "bootstrapper-ui"
    if ui_bin.exists():
        raw = ui_bin.read_bytes()
        assert b"roblox-mac-port" in raw, "bootstrapper-ui does not contain roblox-mac-port string"
    browser_bin = BROWSER / "host"
    if browser_bin.exists():
        raw = browser_bin.read_bytes()
        assert b"roblox-mac-port" in raw, "host binary does not contain roblox-mac-port string"
    so_bin = BROWSER / "libwayland-overlay.so"
    if so_bin.exists():
        raw = so_bin.read_bytes()
        assert b"roblox-mac-port" in raw, "libwayland-overlay.so does not contain roblox-mac-port string"
    print("PASS compiled binaries contain roblox-mac-port identity")

def main():
    test_no_synthetic_red_box()
    test_appimage_icon_setup()
    test_desktop_entries()
    test_run_script_icon_propagation()
    test_browser_host_and_wayland_identity()
    test_bootstrapper_ui_identity()
    test_compiled_binaries()
    print("ALL ICON AND WINDOW IDENTITY CHECKS PASSED!")

if __name__ == "__main__":
    main()
