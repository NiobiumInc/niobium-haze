#!/usr/bin/env bash
# clang-format.sh - format / check first-party C / C++ files.
#
# Usage:
#   scripts/clang-format.sh             # format in place (default)
#   scripts/clang-format.sh --check     # dry-run; exit non-zero on diffs
#   scripts/clang-format.sh --help
#
# Resolves the repo root from `git rev-parse` so the script works from any
# subdirectory. Falls back to `$(dirname BASH_SOURCE)/..` outside a git
# checkout (e.g. when invoked from a nix derivation sandbox where the
# source tree is a copied-in store path with no .git).
#
# Tree set, file globs, and clang-format flags MUST stay in sync with:
#   - .github/workflows/clang-format.yml (CI gate)
#   - flake.nix's clang-format check (nix flake check)
# so local, GH-action, and `nix flake check` runs reproduce each other
# exactly. vendor/ is excluded because submodules carry upstream formatting.

set -euo pipefail

mode=apply
case "${1:-}" in
    --check | --dry-run) mode=check ;;
    --apply | "") mode=apply ;;
    -h | --help)
        sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
        exit 0
        ;;
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

if [[ $mode == check ]]; then
    find src include replay_bridge test \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) \
        -print0 | xargs -0 clang-format --dry-run -Werror
else
    find src include replay_bridge test \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) \
        -print0 | xargs -0 clang-format -i
fi
