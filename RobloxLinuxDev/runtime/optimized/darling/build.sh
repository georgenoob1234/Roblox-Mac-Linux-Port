#!/bin/sh
# Build Darling's runtime at -O2 from the pinned source tree (with this
# project's source fixes) and install it over the stock -O0 Debian libraries.
# The Debian recipe configures CMake with no build type and empty CFLAGS.
# `python3 install.py --restore` returns darling-root to the stock files.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD=$A/src/darling-o2
STAGE=$A/src/darling-o2-stage
PORTABLE=${ROBLOX_MAC_PORTABLE_ROOT:-$(python3 -B "$A/package/portable_runtime.py")}
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
    # Same options as debian/rules, x86_64 only; -O2 without NDEBUG so the
    # runtime's assertions keep their stock behavior.
    (cd "$BUILD" && CFLAGS= CXXFLAGS= CPPFLAGS= LDFLAGS= cmake -G Ninja "$A/src/darling" \
        -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DDEBIAN_PACKAGING=ON -DJSC_UNIFIED_BUILD=ON -DTARGET_i386=OFF \
        -DCOMPONENTS=gui_frameworks,gui_stubs,cli -DENABLE_METAL=ON -DDARLING_NO_CCACHE=ON \
        -DCMAKE_BUILD_TYPE=O2 -DCMAKE_C_FLAGS_O2=-O2 -DCMAKE_CXX_FLAGS_O2=-O2 \
        "-DCMAKE_EXE_LINKER_FLAGS=-B\"$PORTABLE/usr/lib/\"" \
        -DCMAKE_OBJC_FLAGS_O2=-O2 -DCMAKE_OBJCXX_FLAGS_O2=-O2 \
        "-DCMAKE_ASM_FLAGS_O2=" "-DCMAKE_ASM-ATT_FLAGS_O2=" \
        -DVulkan_INCLUDE_DIR="$A/src/Vulkan-Headers-1.3.290/include" \
        -DVulkan_LIBRARY=/usr/lib/libvulkan.so.1) > "$BUILD/configure.log"
else
    cmake -S "$A/src/darling" -B "$BUILD" \
        "-DCMAKE_EXE_LINKER_FLAGS=-B\"$PORTABLE/usr/lib/\"" > "$BUILD/configure.log"
fi
ninja -C "$BUILD" -j "${BUILD_JOBS:-4}" > "$BUILD/build.log"
cc -I "$A/src/darling/src/startup" -I "$BUILD/src/include" \
    -I "$BUILD/src/external/darlingserver/include" \
    -I "$A/src/darling/src/external/darlingserver/include" \
    "$HERE/cli-check.c" -lutil -o "$BUILD/cli-check"
"$BUILD/cli-check"
rm -rf "$STAGE"
DESTDIR=$STAGE ninja -C "$BUILD" install > "$BUILD/install.log"
python3 -B "$HERE/install.py"
