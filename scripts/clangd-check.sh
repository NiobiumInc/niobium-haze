#!/usr/bin/env bash
# clangd-check.sh - run `clangd --check` over haze's first-party .cpp files,
# scanning the diagnostic output for warnings/errors.
#
# Usage:
#   scripts/clangd-check.sh             # default; uses dbuild/ for compile_commands.json
#   BUILD_DIR=build scripts/clangd-check.sh
#   PARALLEL_JOBS=4 scripts/clangd-check.sh
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
# Parallelism: each .cpp is checked in its own clangd invocation, fanned
# out via `xargs -P`. Per-file reports are written to a tmpdir and
# concatenated at the end so the failure output for one file is not
# interleaved with another's. xargs propagates a worker's non-zero
# exit (set on warning/error grep hit) as overall non-zero, which we
# map to a single failed=1.
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

parallel="${PARALLEL_JOBS:-${NIX_BUILD_CORES:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}}"

# Per-file failure reports land here; cleaned on exit. Using a tmpdir
# (rather than serializing stdout via flock) keeps the worker portable
# across linux/darwin and gives deterministic post-mortem output.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

count=$(find src replay_bridge test -name '*.cpp' | wc -l | tr -d ' ')
echo "haze-clangd-check: linting $count first-party .cpp files (-P$parallel)"

# Exported so xargs's `bash -c` children see them. We capture xargs's
# exit code separately because `set -e` would otherwise abort here on
# any worker failure before we get a chance to flush the reports.
export HAZE_CLANGD_BUILD_DIR="$build_dir"
export HAZE_CLANGD_TMPDIR="$tmpdir"

xargs_exit=0
find src replay_bridge test -name '*.cpp' -print0 \
    | xargs -0 -n1 -P"$parallel" bash -c '
        f="$1"
        report=$(clangd --check="$f" --check-locations=false \
                        --compile-commands-dir="$HAZE_CLANGD_BUILD_DIR" 2>&1) || true
        if printf "%s\n" "$report" | grep -qE ": (warning|error):"; then
            # Slot name encodes the source path so the final concat
            # sorts back into a stable order (find walks lexically).
            slot=$(printf "%s" "$f" | tr "/" "_")
            {
                printf "=== %s ===\n" "$f"
                printf "%s\n" "$report"
            } > "$HAZE_CLANGD_TMPDIR/$slot"
            exit 1
        fi
    ' _ || xargs_exit=$?

shopt -s nullglob
for report in "$tmpdir"/*; do
    cat "$report"
done

test "$xargs_exit" = 0
