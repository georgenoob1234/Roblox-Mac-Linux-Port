#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC=$HERE/../../src/darling
SDK=$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
DR=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
OUT=${OUT:-$HERE}
mkdir -p "$OUT/tmp"
export TMPDIR=$OUT/tmp
clang --target=x86_64-apple-macos11 -O2 -mtune=generic -fuse-ld=lld -dynamiclib \
    -isysroot "$DR" -nostdlibinc -Wno-objc-root-class -Wno-nullability-completeness -DTARGET_OS_WASI=0 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -fblocks -isystem "$SDK/usr/include" -F "$SDK/System/Library/Frameworks" \
    -install_name "@rpath/libappkitgaps.dylib" \
    -F "$DR/System/Library/Frameworks" -framework AppKit -framework Foundation -framework QuartzCore -framework WebKit -framework AVFoundation -framework OpenGL -framework CoreGraphics -framework Contacts \
    -F "$DR/System/Library/PrivateFrameworks" -framework Onyx2D -L "$DR/usr/lib/native" -lFreeType \
    -o "$OUT/libappkitgaps.new.dylib" "$HERE/appkit-gaps.m" "$HERE/egl-config.m" "$HERE/elf-exit.c" "$HERE/cursor.m" "$HERE/locked-cursor.m" "$HERE/browser.m" "$HERE/contacts.m" "$HERE/wayland.m" "$HERE/font-coverage.m"
mv "$OUT/libappkitgaps.new.dylib" "$OUT/libappkitgaps.dylib"
echo "built $OUT/libappkitgaps.dylib"
