#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DR=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
SDK=$HERE/../../src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
export TMPDIR=$HERE/../../logs
cc -O2 -shared -fPIC -Wall -Wextra -Wno-misleading-indentation "$HERE/cookies-host.c" -o "$HERE/libcookies-host.so" $(pkg-config --cflags --libs libsoup-3.0 json-glib-1.0) -pthread -Wl,--disable-new-dtags,-rpath,'$ORIGIN/../../browser/lib'
clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib \
    -isysroot "$DR" -nostdlibinc -isystem "$SDK/usr/include" -fblocks -fexceptions -fobjc-exceptions -DTARGET_OS_WASI=0 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -Wno-objc-root-class -Wno-objc-method-access -Wno-nullability-completeness \
    -install_name "@rpath/librbxcfnet.dylib" \
    -F "$SDK/System/Library/Frameworks" -F "$DR/System/Library/Frameworks" -framework Foundation -framework CFNetwork \
    -o "$HERE/librbxcfnet.dylib" "$HERE/cfnet.m" "$HERE/session.m"
echo "built $HERE/librbxcfnet.dylib"
