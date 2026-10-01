#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sh "$HERE/../shims/appkit/build.sh"
"$HERE/ui-build.sh"
sh "$HERE/renderer/build.sh"
python3 -B "$HERE/native/build.py"
sh "$HERE/compat-build.sh"
python3 -B "$HERE/check-launch.py"
printf 'native-x86-port-v1\n' > "$HERE/ready"
