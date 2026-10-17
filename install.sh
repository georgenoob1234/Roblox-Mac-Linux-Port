#!/bin/sh
# One-command, per-user installer bootstrap. Safe to run from a pipe.
# Downloads use curl output options before the URL for POSIX compatibility.
set -eu

REPOSITORY=georgenoob1234/Roblox-Mac-Linux-Port
REF=main
BASE_URL="https://raw.githubusercontent.com/$REPOSITORY/$REF/installer"
EXPECTED_SHA256='200901e40c6a6ca2b3a1bfccf66dbb45fe87eacb39eca7d4a751c1e537790a4a'

fail() {
    echo "install.sh: $*" >&2
    exit 1
}

command -v curl >/dev/null 2>&1 || fail 'curl is required; install curl and retry.'
command -v python3 >/dev/null 2>&1 || fail 'python3 is required; install Python 3 and retry.'
command -v mktemp >/dev/null 2>&1 || fail 'mktemp is required; install coreutils and retry.'
command -v awk >/dev/null 2>&1 || fail 'awk is required; install a POSIX toolset and retry.'
if command -v sha256sum >/dev/null 2>&1; then
    HASH_TOOL=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    HASH_TOOL=shasum
else
    fail 'sha256sum or shasum is required to verify the installer.'
fi

TMPDIR_INSTALL=$(mktemp -d "${TMPDIR:-/tmp}/roblox-mac-linux-port-installer.XXXXXX") ||
    fail 'could not create a private temporary directory.'
cleanup() {
    rm -rf -- "$TMPDIR_INSTALL"
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
umask 077

installer="$TMPDIR_INSTALL/installer.py"
checksum="$TMPDIR_INSTALL/installer.py.sha256"
curl -fsSL --retry 3 -o "$installer" "$BASE_URL/installer.py" ||
    fail "could not download the installer from $BASE_URL/installer.py"
curl -fsSL --retry 3 -o "$checksum" "$BASE_URL/installer.py.sha256" ||
    fail "could not download the installer checksum from $BASE_URL/installer.py.sha256"

actual=$($HASH_TOOL "$installer" | awk '{print tolower($1)}') || fail 'could not hash the downloaded installer.'
case "$HASH_TOOL" in
    sha256sum) expected=$(awk 'NF {print tolower($1); exit}' "$checksum") ;;
    shasum) expected=$(awk 'NF {print tolower($1); exit}' "$checksum") ;;
esac
case "$expected" in
    ''|*[!0-9a-f]*) fail 'the downloaded installer checksum is malformed.' ;;
esac
[ "${#expected}" -eq 64 ] || fail 'the downloaded installer checksum is not SHA-256.'
[ "$actual" = "$EXPECTED_SHA256" ] || fail 'the downloaded installer does not match the pinned SHA-256.'
[ "$actual" = "$expected" ] || fail 'the downloaded installer does not match its checksum file.'

if [ -t 1 ] && [ -t 2 ] && [ -r /dev/tty ] && [ -z "${CI:-}" ]; then
    # The shell reads the curl pipe on stdin; give the Python UI the real
    # terminal so the same command can offer install/update/uninstall menus.
    python3 "$installer" </dev/tty
else
    # CI, redirected output, and other noninteractive callers remain usable.
    python3 "$installer" install --yes
fi
