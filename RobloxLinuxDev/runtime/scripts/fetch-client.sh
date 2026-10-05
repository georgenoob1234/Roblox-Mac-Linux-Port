#!/bin/sh
# Keep $DATA/RobloxPlayer.app at the current x86_64 macOS client version.
# Download into staging first; a failed download must preserve the installed client.
set -eu
DATA=${ROBLOX_MAC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/roblox-mac}
CHANNEL=${ROBLOX_CHANNEL:-MacPlayer}

progress() {
    fd=${ROBLOX_MAC_PROGRESS_FD:-}
    case "$fd" in ''|*[!0-9]*) return 0;; esac
    python3 - "$1" "$2" "$3" "${4:-0}" "${5:-0}" "${6:-true}" <<'PY' >&$fd 2>/dev/null || true
import json, sys
print(json.dumps({"phase": sys.argv[1], "percent": int(sys.argv[2]),
                  "message": sys.argv[3], "bytes_done": int(sys.argv[4]),
                  "bytes_total": int(sys.argv[5]),
                  "cancellable": sys.argv[6].lower() == "true"}, separators=(",", ":")))
PY
}

cancelled() { [ -n "${ROBLOX_MAC_CANCEL_FILE:-}" ] && [ -e "$ROBLOX_MAC_CANCEL_FILE" ]; }

mkdir -p "$DATA"
chmod 700 "$DATA"
query=$(mktemp "$DATA/.version-query.XXXXXX")
query_cleanup() { rm -f -- "$query"; }
trap query_cleanup EXIT

curl -fsSL "https://clientsettingscdn.roblox.com/v2/client-version/$CHANNEL" > "$query"
metadata=$(python3 - "$query" <<'PY'
import json, re, sys
raw = json.loads(open(sys.argv[1], encoding="utf-8").read())
version = raw.get("clientVersionUpload", "")
if not re.fullmatch(r"version-[0-9a-f]+", version):
    raise SystemExit("Invalid client version")
display = ""
for key in ("version", "clientVersion", "versionString", "displayVersion", "clientVersionString"):
    value = raw.get(key)
    if isinstance(value, str) and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", value):
        display = value
        break
print(version)
print(display)
PY
)
V=$(printf '%s\n' "$metadata" | sed -n '1p')
DISPLAY_VERSION=$(printf '%s\n' "$metadata" | sed -n '2p')

if [ "${1:-}" = --version ]; then printf '%s\n' "$V"; exit 0; fi
if [ "${1:-}" = --version-display ]; then printf '%s\n' "$DISPLAY_VERSION"; exit 0; fi

if [ "$(cat "$DATA/.version" 2>/dev/null || true)" = "$V" ] && [ -x "$DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ]; then
    [ -z "$DISPLAY_VERSION" ] || printf '%s' "$DISPLAY_VERSION" > "$DATA/.display-version"
    echo "client up to date: $V" >&2
    exit 0
fi

echo "fetching client $V" >&2
mkdir -p "$DATA"
tmp=$(mktemp -d "$DATA/.dl.XXXXXX")
cleanup() {
    if [ -d "$tmp/previous.app" ] && [ ! -e "$DATA/RobloxPlayer.app" ]; then
        mv "$tmp/previous.app" "$DATA/RobloxPlayer.app"
    fi
    rm -rf -- "$tmp"
}
trap 'query_cleanup; cleanup' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
URL="https://setup.rbxcdn.com/mac/$V-RobloxPlayer.zip"
total=$(curl -fsSLI "$URL" 2>/dev/null | awk 'tolower($1) == "content-length:" {gsub("\r", "", $2); print $2; exit}') || total=
case "$total" in ''|*[!0-9]*) total=0;; esac
progress downloading 25 "Downloading Roblox" 0 "$total"
curl -fsSL -o "$tmp/c.zip" "$URL" &
curl_pid=$!
while kill -0 "$curl_pid" 2>/dev/null; do
    if cancelled; then
        kill "$curl_pid" 2>/dev/null || true
        wait "$curl_pid" 2>/dev/null || true
        progress cancelled 0 "Download cancelled" 0 "$total" false
        exit 125
    fi
    done_bytes=$(wc -c < "$tmp/c.zip" 2>/dev/null || echo 0)
    case "$total" in
        0) percent=-1;;
        *) percent=$((25 + done_bytes * 45 / total)); [ "$percent" -gt 70 ] && percent=70;;
    esac
    progress downloading "$percent" "Downloading Roblox" "$done_bytes" "$total"
    sleep 0.25
done
if ! wait "$curl_pid"; then
    progress failed 0 "Download failed" 0 "$total" false
    exit 1
fi
done_bytes=$(wc -c < "$tmp/c.zip" 2>/dev/null || echo 0)
progress downloading 70 "Download complete" "$done_bytes" "$total"
python3 - "$tmp/c.zip" <<'PY'
import pathlib, stat, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    for entry in archive.infolist():
        path = pathlib.PurePosixPath(entry.filename)
        if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'RobloxPlayer.app' or stat.S_ISLNK(entry.external_attr >> 16):
            raise SystemExit('Unsafe client archive entry')
PY
unzip -qo "$tmp/c.zip" -d "$tmp"
[ -x "$tmp/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] || { echo 'Client archive has no executable' >&2; exit 1; }
[ ! -e "$DATA/RobloxPlayer.app" ] || mv "$DATA/RobloxPlayer.app" "$tmp/previous.app"
if ! mv "$tmp/RobloxPlayer.app" "$DATA/RobloxPlayer.app"; then
    [ ! -e "$tmp/previous.app" ] || mv "$tmp/previous.app" "$DATA/RobloxPlayer.app"
    exit 1
fi
printf '%s' "$V" > "$DATA/.version"
[ -z "$DISPLAY_VERSION" ] || printf '%s' "$DISPLAY_VERSION" > "$DATA/.display-version"
echo "$V" >&2
