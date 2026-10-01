#!/bin/sh
# Reuse the tested Track B batching/IME wrapper and the shared GTK/SDL backend.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${OUT:-$HERE/ui}
mkdir -p "$OUT/tmp"
export TMPDIR=$OUT/tmp
sh "$HERE/../browser/build-protocols.sh"
sh "$HERE/../profiler/build.sh"
c++ -std=c++17 -DRBX_PROFILER -O3 -DNDEBUG -shared -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
 "$HERE/../../native/wayland.cpp" -o "$OUT/libroblox-wayland.new.so" \
 $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 json-glib-1.0 sdl3 wayland-client wayland-egl xcursor) "$HERE/../browser/protocols/relative-pointer.o" "$HERE/../browser/protocols/pointer-constraints.o" "$HERE/../profiler/build/libprofiler.a" -lGL -lEGL -lX11 -Wl,--disable-new-dtags,-rpath,'$ORIGIN/../../browser/lib'
c++ -std=c++17 -DRBX_PROFILER -O2 -DTRACKB_WAYLAND_CHECK -Wno-deprecated-declarations \
 "$HERE/../../native/wayland.cpp" -o "$OUT/wayland-check" \
 $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 json-glib-1.0 sdl3 wayland-client wayland-egl xcursor) "$HERE/../browser/protocols/relative-pointer.o" "$HERE/../browser/protocols/pointer-constraints.o" "$HERE/../profiler/build/libprofiler.a" -lGL -lEGL -lX11
"$OUT/wayland-check"
mv "$OUT/libroblox-wayland.new.so" "$OUT/libroblox-wayland.so"
