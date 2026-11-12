#!/bin/sh
# cache-current.sh APP CACHE
# Succeeds when the translated-shader cache in CACHE was prepared from APP's shader pack.
# prepare-cache.py records the pack's SHA-256 in report.json; a cache prepared for another
# client lacks the shaders that changed since. Hashing the pack takes about 10 ms.
set -eu
pack=$1/Contents/Resources/shaders/shaders_metal_osx.pack
report=$2/report.json
[ -f "$pack" ] && [ -f "$report" ] || exit 1
hash=$(sha256sum "$pack")
grep -q "\"pack_sha256\": \"${hash%% *}\"" "$report"
