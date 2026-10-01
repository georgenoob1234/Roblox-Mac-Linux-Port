#!/bin/sh
# shaders_metal_osx.pack -> out/air/*.air -> out/spv/*.spv + *.json through metal2vulkan.
# Prints the pass/fail count and the distinct failure reasons. Re-run after every client update.
#   ./translate-all.sh [path/to/RobloxPlayer.app]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
APP=${1:-${ROBLOX_MAC_APP:-$ROOT/RobloxPlayer.app}}
M2V=${ROBLOX_MAC_TRANSLATOR:-$ROOT/metal2vulkan/target/release/metal2vulkan}
[ ! -x "$HERE/../usr/bin/metal2vulkan" ] || M2V=$HERE/../usr/bin/metal2vulkan
for tool in python3 llvm-dis spirv-val; do
    command -v "$tool" >/dev/null || { echo "missing shader preparation tool: $tool" >&2; exit 1; }
done
[ -x "$M2V" ] || (cd "$ROOT/metal2vulkan" && cargo build --release --features serde --bin metal2vulkan)

OUT=${ROBLOX_MAC_SHADER_BUILD:-$HERE/out}
rm -rf "$OUT/air" "$OUT/spv" "$OUT/repros"; mkdir -p "$OUT/air" "$OUT/spv" "$OUT/repros"
python3 "$HERE/pack2air.py" "$APP/Contents/Resources/shaders/shaders_metal_osx.pack" "$OUT/air"

export METAL2VULKAN_REPRO_DIR=$OUT/repros
export TMPDIR=$OUT/tmp
mkdir -p "$TMPDIR"
find "$OUT/air" -maxdepth 1 -name '*.air' -print0 | xargs -0 -P"$(nproc)" -I{} sh -c '
    b=$(basename "$1" .air)
    python3 "$2/translate-one.py" "$3" "$1" "$4/spv/$b"
' sh {} "$HERE" "$M2V" "$OUT" | sort | uniq -c
echo '--- failure reasons:'
grep -h 'metal2vulkan: ' "$OUT"/spv/*.out | grep -v 'wrote\|PASS\|auto-detected\|FALLBACK' \
    | sed 's/[0-9]\+/N/g; s/;.*//' | sort | uniq -c | sort -rn
# The translator omits the terminating signal. Retry one affected input directly
# so diagnostics distinguish SIGILL, SIGSEGV and SIGKILL under the same limits.
failed=$(grep -l 'llvm-dis killed by signal' "$OUT"/spv/*.out | head -n 1)
if [ -n "$failed" ]; then
    python3 "$HERE/../scripts/diagnostics.py" --llvm-check "$OUT/air/$(basename "$failed" .out).air"
fi
