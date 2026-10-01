#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORTABLE=${ROBLOX_MAC_PORTABLE_ROOT:-$(python3 -B "$HERE/../package/portable_runtime.py")}
export TMPDIR="$HERE/../logs"
sh "$HERE/build-protocols.sh"
c++ -B"$PORTABLE/usr/lib/" -march=x86-64 -mtune=generic -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations "$HERE/host.cpp" -o "$HERE/roblox-gtk.bin" $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 json-glib-1.0) -lX11
c++ -std=c++17 -O2 -shared -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations "$HERE/wayland.cpp" -o "$HERE/libroblox-wayland.so" $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 json-glib-1.0 sdl3 wayland-client wayland-egl xcursor) "$HERE/protocols/relative-pointer.o" "$HERE/protocols/pointer-constraints.o" -lX11 -Wl,--disable-new-dtags,-rpath,'$ORIGIN/lib'
