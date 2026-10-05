#!/bin/sh
# Register this release as a user-local handler for Roblox browser links.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
RUN=$HERE/run.sh
DATA_HOME=${XDG_DATA_HOME:-${HOME:?}/.local/share}
CONFIG_HOME=${XDG_CONFIG_HOME:-${HOME:?}/.config}
APP_DIR=$DATA_HOME/applications
ICON_DIR=$DATA_HOME/icons/hicolor/256x256/apps
ICON_SOURCE=$HERE/icon.png
ICON_PATH=$ICON_DIR/roblox-mac-port.png
SETTINGS_ICON_SOURCE=$HERE/icon_bw.png
SETTINGS_ICON_PATH=$ICON_DIR/roblox-mac-port-settings.png
DESKTOP=roblox-mac-port.desktop
DESKTOP_PATH=$APP_DIR/$DESKTOP
SETTINGS_DESKTOP=roblox-mac-port-settings.desktop
SETTINGS_DESKTOP_PATH=$APP_DIR/$SETTINGS_DESKTOP
MIMEAPPS=$CONFIG_HOME/mimeapps.list

escape_exec() {
    # Desktop Exec fields use backslash escaping for separators and quotes.
    printf '%s' "$1" | sed 's/[\\"]/[\\&]/g; s/ /\\ /g; s/	/\\	/g'
}

install_handler() {
    [ -f "$ICON_SOURCE" ] || { echo "Missing icon file: $ICON_SOURCE" >&2; exit 1; }
    [ -f "$SETTINGS_ICON_SOURCE" ] || { echo "Missing icon file: $SETTINGS_ICON_SOURCE" >&2; exit 1; }
    mkdir -p "$APP_DIR" "$ICON_DIR"
    exec_path=$(escape_exec "$RUN")
    umask 022
    cat > "$DESKTOP_PATH" <<EOF
[Desktop Entry]
Name=Roblox (Mac Linux Port)
Comment=Launch the Intel macOS Roblox client
Type=Application
Exec=$exec_path %u
Icon=roblox-mac-port
Terminal=false
StartupNotify=true
Categories=Game;
MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;
EOF
    cp "$ICON_SOURCE" "$ICON_PATH"
    cat > "$SETTINGS_DESKTOP_PATH" <<EOF
[Desktop Entry]
Name=Roblox Launcher
Comment=Open the Mac O’ Blox launcher
Type=Application
Exec=$exec_path --launcher
Icon=roblox-mac-port-settings
Terminal=false
StartupNotify=true
Categories=Game;
EOF
    cp "$SETTINGS_ICON_SOURCE" "$SETTINGS_ICON_PATH"
    chmod 644 "$DESKTOP_PATH" "$ICON_PATH" "$SETTINGS_DESKTOP_PATH" "$SETTINGS_ICON_PATH"
    if command -v xdg-mime >/dev/null 2>&1; then
        xdg-mime default "$DESKTOP" x-scheme-handler/roblox || echo 'Warning: xdg-mime could not register roblox.' >&2
        xdg-mime default "$DESKTOP" x-scheme-handler/roblox-player || echo 'Warning: xdg-mime could not register roblox-player.' >&2
    else
        echo 'Warning: xdg-mime is unavailable; install it or set mimeapps.list manually.' >&2
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APP_DIR" >/dev/null 2>&1 || echo 'Warning: update-desktop-database failed.' >&2
    else
        echo 'Note: update-desktop-database is unavailable; the menu may refresh later.' >&2
    fi
    echo "Installed $DESKTOP_PATH and $SETTINGS_DESKTOP_PATH"
    echo 'Verify with: xdg-mime query default x-scheme-handler/roblox'
}

uninstall_handler() {
    rm -f -- "$DESKTOP_PATH" "$ICON_PATH" "$SETTINGS_DESKTOP_PATH" "$SETTINGS_ICON_PATH"
    if [ -f "$MIMEAPPS" ]; then
        tmp=$MIMEAPPS.tmp.$$
        sed -e 's/roblox-mac-port\.desktop;//g' \
            -e 's/;roblox-mac-port\.desktop//g' \
            -e 's/=roblox-mac-port\.desktop$/=/' \
            -e '/^x-scheme-handler\/roblox=$/d' \
            -e '/^x-scheme-handler\/roblox-player=$/d' "$MIMEAPPS" > "$tmp"
        chmod 600 "$tmp" 2>/dev/null || true
        mv -f -- "$tmp" "$MIMEAPPS"
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
    fi
    echo "Removed $DESKTOP_PATH, $SETTINGS_DESKTOP_PATH and its own MIME entries"
}

case "${1:-install}" in
    install) [ "$#" -le 1 ] || { echo 'Usage: install-uri-handler.sh [install|uninstall]' >&2; exit 2; }; install_handler;;
    uninstall) [ "$#" -eq 1 ] || { echo 'Usage: install-uri-handler.sh [install|uninstall]' >&2; exit 2; }; uninstall_handler;;
    *) echo 'Usage: install-uri-handler.sh [install|uninstall]' >&2; exit 2;;
esac
