#!/bin/sh
# CI lint helper — run from the repository root.
# Fast syntax and consistency checks only; no compilation, no downloads.
set -eu

fail=0

# --- Shell syntax (sh -n for every tracked .sh) --------------------------------------------
echo '== Shell syntax =='
shells=$(git ls-files '*.sh')
if [ -n "$shells" ]; then
    for f in $shells; do
        sh -n "$f" || { echo "FAIL: sh -n $f" >&2; fail=1; }
    done
else
    echo '(no tracked shell scripts)'
fi

# --- Python syntax --------------------------------------------------------------------------
echo '== Python syntax =='
pys=$(git ls-files '*.py')
if [ -n "$pys" ]; then
    for f in $pys; do
        python3 -m py_compile "$f" || { echo "FAIL: python3 -m py_compile $f" >&2; fail=1; }
    done
else
    echo '(no tracked Python sources)'
fi

# --- JSON validation ------------------------------------------------------------------------
echo '== JSON validation =='
jsons=$(git ls-files '*.json')
if [ -n "$jsons" ]; then
    for f in $jsons; do
        python3 -c "import json; json.load(open('$f'))" || {
            echo "FAIL: JSON parse $f" >&2; fail=1
        }
    done
else
    echo '(no tracked JSON files)'
fi

# --- Workflow YAML (actionlint, pinned) -----------------------------------------------------
echo '== Workflow YAML =='
ACTIONLINT_VERSION=1.7.7
if command -v actionlint >/dev/null 2>&1; then
    actionlint || fail=1
else
    url="https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz"
    if curl -sfL --retry 3 -o /tmp/actionlint.tar.gz "$url"; then
        tar -xzf /tmp/actionlint.tar.gz -C /tmp actionlint
        chmod +x /tmp/actionlint
        /tmp/actionlint -color -config-file .actionlint.yaml || fail=1
        rm -f /tmp/actionlint /tmp/actionlint.tar.gz
    else
        echo 'WARN: actionlint download failed; falling back to python3 -c yaml safe_load'
        python3 -c '
import sys, yaml
for f in sys.argv[1:]:
    try:
        yaml.safe_load(open(f))
    except Exception as e:
        print(f"FAIL: {f}: {e}", file=sys.stderr)
        sys.exit(1)
' .github/workflows/*.yml || fail=1
    fi
fi

# --- Darling overlay consistency ------------------------------------------------------------
echo '== Darling overlay consistency =='
overlay=RobloxLinuxDev/third_party/darling-overlay
if [ -d "$overlay" ]; then
    (cd "$overlay" && find . -type f) | while read -r f; do
        rel=${f#./}
        if [ ! -f "$overlay/$rel" ]; then
            echo "FAIL: overlay entry missing: $rel" >&2
            fail=1
        fi
    done
    echo "Darling overlay: $(cd "$overlay" && find . -type f | wc -l) file(s) present"
else
    echo '(no Darling overlay directory)'
fi

if [ "$fail" -ne 0 ]; then
    echo 'FAIL: one or more lint checks failed' >&2
    exit 1
fi
echo 'All lint checks passed.'
