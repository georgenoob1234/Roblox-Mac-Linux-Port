#!/bin/sh
# Verify that a release tag is well-formed and does not already exist.
#
# Usage: check-tag.sh <tag>
#
# Must run inside a git checkout of the target repository with `origin`
# configured, and with gh + GH_TOKEN available for the release check.
# Exits non-zero whenever the tag must not be used, so a long build or a
# publish never overwrites an existing tag or release.
set -eu

[ "$#" -eq 1 ] || { echo 'usage: check-tag.sh <tag>' >&2; exit 2; }
TAG=$1

fail() { echo "::error::check-tag: $*" >&2; exit 1; }

# Format: a version tag (vX.Y.Z with optional pre-release suffix).
printf '%s' "$TAG" | grep -qE '^v[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$' \
    || fail "tag '$TAG' does not match vX.Y.Z or vX.Y.Z-suffix"

# Remote tag check via ls-remote: works without fetching the repository.
# Exit codes: 0 = tag exists, 2 = no matching tag, other = remote error.
# Guard with `|| rc=$?` so set -e does not abort on exit 2 (tag absent).
rc=0
git ls-remote --tags --exit-code origin "refs/tags/$TAG" >/dev/null 2>&1 || rc=$?
case $rc in
    0) fail "tag '$TAG' already exists — refusing to overwrite it" ;;
    2) : ;;
    *) fail "cannot verify tag '$TAG' against origin (git ls-remote exit $rc)" ;;
esac

# GitHub Release check with explicit HTTP status: 200 = exists, 404 =
# absent, anything else (auth, rate limit, network) = refuse to continue.
http=$(gh api -i "repos/$GITHUB_REPOSITORY/releases/tags/$TAG" 2>&1 | head -n 1)
case " $http " in
    *' 200 '*) fail "release '$TAG' already exists — refusing to overwrite it" ;;
    *' 404 '*) : ;;
    *) fail "cannot verify release '$TAG' (GitHub API: $http)" ;;
esac

echo "Tag $TAG is well-formed and unused."
