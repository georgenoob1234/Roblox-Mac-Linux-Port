#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORTABLE=${ROBLOX_MAC_PORTABLE_ROOT:-$(python3 -B "$HERE/portable_runtime.py")}
gcc -B"$PORTABLE/usr/lib/" -march=x86-64 -mtune=generic -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
    "$HERE/bootstrapper-ui.c" -o "$HERE/bootstrapper-ui" \
    $(pkg-config --cflags --libs gtk+-3.0 json-glib-1.0) \
    -Wl,-rpath,'$ORIGIN/../../browser/lib'
