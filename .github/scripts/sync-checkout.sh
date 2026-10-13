#!/bin/sh
# Safely synchronize the persistent CI checkout to an exact trusted commit.
#
# Usage: sync-checkout.sh <repo-url> <commit-sha> <checkout-dir>
#
# The build's data directory lives inside the checkout, and the runtime
# derives Unix-domain socket paths from it (107-byte sun_path limit), so
# builds must use the short dedicated checkout /home/runner/r instead of
# the deep default GitHub Actions workspace.  That checkout persists
# between runs, which this script keeps safe:
#
#   - Fetches and checks out the exact pinned commit SHA (never a moving
#     branch ref), so the build, packaging and publishing all operate on
#     the same commit.
#   - Preserves ignored dependency trees and build outputs
#     (RobloxLinuxDev/.build, .downloads, metal2vulkan, ...) — no
#     `git reset --hard`, no `git clean`, no symlink relocation.
#   - Fails clearly instead of overwriting unexpected tracked
#     modifications in the persistent working tree.
#   - Stores no credentials: the repository is public, so the remote is
#     fetched anonymously over HTTPS.
set -eu

[ "$#" -eq 3 ] || { echo 'usage: sync-checkout.sh <repo-url> <commit-sha> <checkout-dir>' >&2; exit 2; }
REPO_URL=$1
SHA=$2
DIR=$3

fail() { echo "::error::sync-checkout: $*" >&2; exit 1; }

case $SHA in
    ''|*[!0-9a-f]*) fail "invalid commit SHA '$SHA'" ;;
esac
[ "${#SHA}" -eq 40 ] || fail "commit SHA must be 40 hex characters, got '$SHA'"

if [ -e "$DIR" ] && [ ! -d "$DIR" ]; then
    fail "$DIR exists and is not a directory; refusing to touch it"
fi

if [ ! -d "$DIR/.git" ]; then
    if [ -e "$DIR" ]; then
        fail "$DIR exists but is not a git repository; refusing to touch it"
    fi
    echo "Cloning $REPO_URL into $DIR (first use)"
    GIT_TERMINAL_PROMPT=0 git clone "$REPO_URL" "$DIR"
fi

cd "$DIR"

# Refuse a persistent checkout whose remote points elsewhere (a leftover
# fork or personal clone would fetch history the pinned SHA never came
# from).  Compare URL ignoring a trailing ".git".
remote_url=$(git remote get-url origin)
strip() { printf '%s' "$1" | sed -e 's/\.git$//' -e 's:/*$::'; }
if [ "$(strip "$remote_url")" != "$(strip "$REPO_URL")" ]; then
    fail "origin '$remote_url' does not match '$REPO_URL'; refusing to use this checkout"
fi

# Tracked modifications are never overwritten: abort and let a human
# decide.  Untracked files ('??') are tolerated — git itself refuses to
# check out over them — and ignored build outputs do not show up here.
status=$(git status --porcelain)
if [ -n "$status" ] && printf '%s\n' "$status" | grep -qv '^??'; then
    echo "::error::persistent checkout $DIR has tracked modifications:" >&2
    printf '%s\n' "$status" | grep -v '^??' >&2
    fail "resolve them manually (never via git reset --hard); refusing to overwrite"
fi

echo "Fetching commit $SHA"
GIT_TERMINAL_PROMPT=0 git fetch --no-tags origin "$SHA" \
    || fail "cannot fetch commit $SHA from origin"
git cat-file -e "$SHA^{commit}" \
    || fail "$SHA is not available as a commit in the checkout"

# Detached checkout of the pinned SHA.  With a clean tracked tree this
# never discards work, and git refuses to clobber untracked files.
git checkout --detach "$SHA" \
    || fail "cannot check out $SHA (local files would be overwritten)"

[ "$(git rev-parse HEAD)" = "$SHA" ] || fail "HEAD is not at $SHA after checkout"
echo "Persistent checkout ready: $DIR at $SHA"
