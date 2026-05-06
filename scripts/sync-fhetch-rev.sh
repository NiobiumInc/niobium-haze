#!/usr/bin/env bash
# Sync the niobium-fhetch flake input rev in flake.lock to whatever rev
# the vendor/niobium-fhetch submodule is currently checked out at in the
# haze worktree's index. Run after bumping the submodule so the two
# sources of truth stay in lockstep; the flake-check CI job fails if
# they disagree.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

submodule_rev=$(git ls-tree HEAD vendor/niobium-fhetch | awk '{print $3}')
if [[ -z $submodule_rev ]]; then
    echo "error: vendor/niobium-fhetch is not registered as a submodule in HEAD" >&2
    exit 1
fi

echo "syncing flake.lock niobium-fhetch-src rev -> $submodule_rev"
nix flake lock --override-input niobium-fhetch-src \
    "git+ssh://git@github.com/NiobiumInc/niobium-fhetch.git?rev=${submodule_rev}&submodules=1"
