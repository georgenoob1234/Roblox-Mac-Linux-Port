#!/bin/sh
# Verify the release archive contents before it is uploaded anywhere.
#
# Usage: verify-release-archive.sh <RobloxLinuxRelease.tar.gz>
#
# The release must contain exactly one top-level directory
# (RobloxLinuxRelease) holding exactly the distributable files staged by
# RobloxLinuxDev/runtime/package/build-release.sh — no development files,
# build caches, source dependency checkouts, downloaded Roblox client
# contents, cookies or other private data.
#
# Keep REQUIRED in sync with build-release.sh: a deliberately changed
# release layout must update this list, and anything unexpected fails.
set -eu

[ "$#" -eq 1 ] || { echo 'usage: verify-release-archive.sh <archive>' >&2; exit 2; }
ARCHIVE=$1

fail() { echo "::error::verify-release-archive: $*" >&2; exit 1; }

test -f "$ARCHIVE" || fail "archive not found: $ARCHIVE"

# Exactly one top-level directory, named RobloxLinuxRelease.
top=$(tar -tzf "$ARCHIVE" | sed 's#/.*##' | sort -u)
[ "$top" = 'RobloxLinuxRelease' ] || {
    fail "expected a single top-level directory 'RobloxLinuxRelease', got: $(printf '%s' "$top" | tr '\n' ' ')"
}

# Files staged by build-release.sh (including the hidden .gitignore).
REQUIRED='
RobloxLinux.AppImage
run.sh
update-roblox.sh
bootstrap.sh
bootstrap.py
install-uri-handler.sh
icon.png
icon_bw.png
README.md
FFlags.json
.gitignore
'

files=$(tar -tzf "$ARCHIVE" | grep -v '/$' | sort)
expected=$(printf '%s' "$REQUIRED" | sed -e '/^$/d' -e 's#^#RobloxLinuxRelease/#' | sort)

for f in $REQUIRED; do
    [ -n "$f" ] || continue
    printf '%s\n' "$files" | grep -qx "RobloxLinuxRelease/$f" \
        || fail "required file missing from archive: RobloxLinuxRelease/$f"
done

# Exact set match: anything extra must be reviewed before distribution.
# This also rejects the development AppImage name, nested directories and
# any accidental inclusion of the whole development repository.
if [ "$files" != "$expected" ]; then
    echo 'Unexpected archive contents (vs. expected file list):' >&2
    printf '%s\n' "$files" >&2
    fail "archive file list does not exactly match the release layout; fix packaging or update REQUIRED in this script"
fi

# Belt and braces for the exact match above: private and development
# content must never be distributed, even under renamed paths.
forbidden_re='DO_NOT_SHARE|cookie|\.build|\.downloads|\.cargo|metal2vulkan|third_party|darling|AppDir|__pycache__|prefix|profile|\.log$|roblox-mac-x86_64|logs|tmp|deps|secrets?'
offending=$(printf '%s\n' "$files" | grep -Ei "$forbidden_re" || true)
[ -z "$offending" ] || fail "archive contains forbidden paths: $(printf '%s' "$offending" | tr '\n' ' ')"

echo 'Archive verification passed:'
echo "$files"
