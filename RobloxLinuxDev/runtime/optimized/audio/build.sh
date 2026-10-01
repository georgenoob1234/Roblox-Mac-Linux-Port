#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUT=${OUT:-$HERE/build}
SDK=$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
SRC=$A/src/darling/src/CoreAudio
mkdir -p "$OUT/tmp"
export TMPDIR=$OUT/tmp
for PROBE in device-check fmod-unit-check host-clock-check capture-check; do
clang++ --target=x86_64-apple-macos11 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -DTARGET_OS_WASI=0 -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign \
 -isysroot "$A/darling-root" -nostdinc++ -isystem "$A/src/darling/src/external/libcxx/include" \
 -isystem "$SDK/usr/include" -I "$SRC/include" -I "$A/src/darling/src/frameworks/CoreServices/include" -F "$SDK/System/Library/Frameworks" \
 -framework CoreAudio -framework CoreFoundation -framework AudioToolbox "$HERE/$PROBE.cpp" -o "$OUT/$PROBE"
done
mkdir -p "$OUT/CoreAudio.framework/Versions/A" "$OUT/include"
# Copy only Pulse headers; never put Linux libc headers ahead of Darwin's SDK.
cp -a /usr/include/pulse "$OUT/include/"
clang++ --target=x86_64-apple-macos11 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -DTARGET_OS_WASI=0 -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -dynamiclib \
 -isysroot "$A/darling-root" -nostdinc++ -isystem "$A/src/darling/src/external/libcxx/include" \
 -isystem "$SDK/usr/include" -I "$SRC/include" -I "$OUT/include" -F "$SDK/System/Library/Frameworks" \
 -I "$A/src/darling/src/frameworks/CoreServices/include" \
 -L "$A/darling-root/usr/lib/native" -lpulse -framework CoreFoundation \
 -Wno-nullability-completeness \
 -compatibility_version 1.0.0 -install_name /System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio \
 "$HERE"/CoreAudio/*.cpp "$HERE"/CoreAudio/*.mm "$HERE"/CoreAudio/pulse/*.cpp \
 -o "$OUT/CoreAudio.framework/Versions/A/CoreAudio"
ln -sfn A "$OUT/CoreAudio.framework/Versions/Current"
ln -sfn Versions/Current/CoreAudio "$OUT/CoreAudio.framework/CoreAudio"
COMP=$SRC/CoreAudioComponent
set --
for f in AUPublic/AUBase/AUBase.cpp AUPublic/AUBase/AUDispatch.cpp AUPublic/AUBase/AUInputElement.cpp AUPublic/AUBase/AUOutputElement.cpp AUPublic/AUBase/AUPlugInDispatch.cpp AUPublic/AUBase/AUScopeElement.cpp AUPublic/AUBase/ComponentBase.cpp AUPublic/Utility/AUBuffer.cpp PublicUtility/CAAudioChannelLayout.cpp PublicUtility/CAAudioChannelLayoutObject.cpp PublicUtility/CABufferList.cpp PublicUtility/CADebugger.cpp PublicUtility/CAStreamBasicDescription.cpp PublicUtility/CAHostTimeBase.cpp PublicUtility/CAVectorUnit.cpp; do set -- "$@" "$COMP/$f"; done
mkdir -p "$OUT/CoreAudio.component/Contents/MacOS"
clang++ --target=x86_64-apple-macos11 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -DTARGET_OS_WASI=0 -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -bundle -fvisibility=hidden -DCA_BASIC_AU_FEATURES=1 \
 -isysroot "$A/darling-root" -nostdinc++ -isystem "$A/src/darling/src/external/libcxx/include" \
 -isystem "$SDK/usr/include" -I "$SRC/include" -I "$A/src/darling/src/frameworks/CoreServices/include" -F "$SDK/System/Library/Frameworks" \
 -I "$COMP" -I "$COMP/PublicUtility" -I "$COMP/AUPublic/AUBase" -I "$COMP/AUPublic/Utility" \
 -I "$A/src/darling/src/frameworks/CoreServices/include" \
 -framework CoreAudio -framework CoreFoundation -framework AudioToolbox -Wno-nullability-completeness \
 "$HERE/AUHAL.cpp" "$HERE/DefaultOutputAU.cpp" "$HERE/SystemOutputAU.cpp" "$@" \
 -o "$OUT/CoreAudio.component/Contents/MacOS/CoreAudio"
cp "$COMP/Info.plist" "$OUT/CoreAudio.component/Contents/Info.plist"
