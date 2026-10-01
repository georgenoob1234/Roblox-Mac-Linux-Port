#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=$HERE/protocols
mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/build.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
for name in relative-pointer pointer-constraints; do
    xml=$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/$name/$name-unstable-v1.xml
    wayland-scanner client-header "$xml" "$WORK/$name.h"
    wayland-scanner private-code "$xml" "$WORK/$name.c"
    cc -fPIC -c "$WORK/$name.c" -o "$WORK/$name.o"
    for ext in h c o; do
        if ! cmp -s "$WORK/$name.$ext" "$OUT/$name.$ext"; then mv "$WORK/$name.$ext" "$OUT/$name.$ext"; fi
    done
done
