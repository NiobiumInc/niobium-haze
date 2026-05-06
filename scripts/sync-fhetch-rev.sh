#!/usr/bin/env bash
# Sync the niobium-fhetch-src flake input rev in flake.lock to
# whatever rev the vendor/niobium-fhetch submodule is checked out at
# in the haze worktree's index. Run after bumping the submodule so
# the two sources of truth stay in lockstep; the flake-check CI job
# fails if they disagree.
#
# Two paths:
#
# - With nix on PATH, defer to `nix flake lock --override-input`. This
#   refetches and recomputes every derived field (narHash, lastModified,
#   revCount) precisely.
#
# - Without nix, fall back to a jq-driven JSON edit: update `rev` and
#   drop `narHash` from the lock entry. The next nix invocation
#   (e.g. CI's `nix flake check`) refetches and fills `narHash` back
#   in, so non-nix contributors can update the lock without installing
#   nix locally. The CI rev-equality gate keys on `rev` only, so the
#   intermediate lock state is enough to merge.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

submodule_rev=$(git ls-tree HEAD vendor/niobium-fhetch | awk '{print $3}')
if [[ -z $submodule_rev ]]; then
    echo "error: vendor/niobium-fhetch is not registered as a submodule in HEAD" >&2
    exit 1
fi

echo "syncing flake.lock niobium-fhetch-src rev -> $submodule_rev"

if command -v nix >/dev/null 2>&1; then
    nix flake lock --override-input niobium-fhetch-src \
        "git+ssh://git@github.com/NiobiumInc/niobium-fhetch.git?rev=${submodule_rev}&submodules=1"
    exit 0
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "error: need either nix or jq on PATH to update flake.lock" >&2
    exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

jq --arg rev "$submodule_rev" \
    '.nodes."niobium-fhetch-src".locked.rev = $rev
     | del(.nodes."niobium-fhetch-src".locked.narHash)' \
    flake.lock > "$tmp"

if cmp -s "$tmp" flake.lock; then
    echo "flake.lock already in sync"
    exit 0
fi

mv "$tmp" flake.lock
trap - EXIT

echo "flake.lock updated (narHash cleared; nix will recompute on next invocation)"
