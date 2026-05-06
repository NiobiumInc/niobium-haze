#!/usr/bin/env bash
# clangd-check.sh - run `clangd --check` over haze's first-party .cpp files,
# scanning the diagnostic output for warnings/errors.
#
# Usage:
#   scripts/clangd-check.sh             # default; uses dbuild/ for compile_commands.json
#   BUILD_DIR=build scripts/clangd-check.sh
#   scripts/clangd-check.sh --help
#
# Resolves the repo root via `git rev-parse` (falls back to the script's
# `$(dirname BASH_SOURCE)/..` outside a git checkout).
#
# Why grep instead of `clangd --check` exit status: clangd exits zero on
# warnings without an explicit grep pass; we want any `warning:` /
# `error:` line in the report to fail the gate so .clangd's
# Diagnostics block (UnusedIncludes / MissingIncludes: Strict) drives
# include hygiene through this gate. `--check-locations=false` skips
# the per-token feature-test sweep that prints spurious tweak failures
# on `break`/`continue` tokens in functions with switches.
#
# Requires compile_commands.json — run `make build` locally, or rely on
# the flake derivation's cmake setup hook which configures one before
# invoking this script.

set -euo pipefail

case "${1:-}" in
    -h | --help)
        sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
        exit 0
        ;;
    "") ;;
    *)
        printf '%s: unknown argument: %s\n' "$(basename "$0")" "$1" >&2
        exit 2
        ;;
esac

if root=$(git rev-parse --show-toplevel 2>/dev/null); then
    cd "$root"
else
    cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

build_dir="${BUILD_DIR:-dbuild}"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    printf '%s: no compile_commands.json under %s/. Run `make build` first.\n' \
        "$(basename "$0")" "$build_dir" >&2
    exit 1
fi

# clangd reads compile_commands.json from --compile-commands-dir or by
# walking up from the .cpp's path. The latter works when build/ is at
# the repo root; we set it explicitly so callers using a non-default
# BUILD_DIR get the right database.
files=$(find src replay_bridge test -name '*.cpp')
count=$(printf '%s\n' "$files" | grep -c .)
echo "haze-clangd-check: linting $count first-party .cpp files"
failed=0
for f in $files; do
    report=$(clangd --check="$f" --check-locations=false \
                    --compile-commands-dir="$build_dir" 2>&1) || true
    if echo "$report" | grep -qE ': (warning|error):'; then
        echo "=== $f ==="
        echo "$report"
        failed=1
    fi
done
test "$failed" = 0
