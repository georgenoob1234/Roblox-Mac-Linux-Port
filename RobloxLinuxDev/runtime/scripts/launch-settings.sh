#!/bin/sh
# Called under the instance lock, before starting the client.
set -eu
FLAGS=$1 APP=$2
shift 2
if [ -n "$FLAGS" ]; then
    python3 - "$FLAGS" "$APP" <<'PY'
import json, os, pathlib, sys, tempfile
flags, app = map(pathlib.Path, sys.argv[1:])
data = json.loads(flags.read_text())
if not isinstance(data, dict) or any(not isinstance(v, (str, bool, int)) for v in data.values()):
    raise SystemExit('FFlags.json must be an object containing string, boolean or integer values')
target = app / 'Contents/MacOS/ClientSettings/ClientAppSettings.json'
target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.NamedTemporaryFile(mode='w', dir=target.parent, delete=False) as f:
    json.dump(data, f, indent=2)
os.replace(f.name, target)
PY
fi
exec "$@"
