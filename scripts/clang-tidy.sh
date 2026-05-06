#!/usr/bin/env bash
# clang-tidy.sh - run clang-tidy with --warnings-as-errors='*' over haze's
# first-party .cpp files.
#
# Usage:
#   scripts/clang-tidy.sh             # default; uses build/ for compile_commands.json
#   BUILD_DIR=dbuild scripts/clang-tidy.sh
#   PARALLEL_JOBS=4 scripts/clang-tidy.sh
#   scripts/clang-tidy.sh --help
#
# Resolves the repo root from `git rev-parse` (falls back to the script's
# `$(dirname BASH_SOURCE)/..` outside a git checkout, e.g. inside a nix
# derivation sandbox where the source tree has no .git).
#
# Tree set, file globs, and `--warnings-as-errors='*'` MUST stay in sync
# with the haze-clang-tidy flake check that wraps this script. The lint
# scope is "first-party .cpp"; headers are linted transitively via the
# .cpp that includes them. Requires compile_commands.json — run
# `make build` locally, or rely on the flake derivation's cmake setup
# hook which configures one before invoking this script.

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

build_dir="${BUILD_DIR:-build}"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    printf '%s: no compile_commands.json under %s/. Run `make build` first.\n' \
        "$(basename "$0")" "$build_dir" >&2
    exit 1
fi

parallel="${PARALLEL_JOBS:-${NIX_BUILD_CORES:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}}"

find src replay_bridge test -name '*.cpp' -print0 \
    | xargs -0 -n4 -P"$parallel" clang-tidy -p "$build_dir" \
        --warnings-as-errors='*' --quiet
