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
#   refetches and recomputes every derived field (narHash,
#   lastModified, revCount) precisely.
#
# - Without nix, fall back to a python3 JSON edit: update `rev` and
#   drop `narHash`. The next nix invocation (e.g. CI's `nix flake
#   check`) refetches and refills `narHash`, so non-nix contributors
#   can update the lock without installing nix locally. The CI
#   rev-equality gate keys on `rev` only, so the intermediate lock
#   state is enough to merge. python3 is used (instead of jq) because
#   it is more widely pre-installed; its JSON dump with
#   indent=2/sort_keys=True is byte-identical to what nix writes,
#   keeping the diff minimal.
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
        "git+https://github.com/NiobiumInc/niobium-fhetch.git?rev=${submodule_rev}&submodules=1"
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: need either nix or python3 on PATH to update flake.lock" >&2
    exit 1
fi

python3 - "$submodule_rev" <<'PYEOF'
import json
import pathlib
import sys

rev = sys.argv[1]
path = pathlib.Path("flake.lock")
data = json.loads(path.read_text())
locked = data["nodes"]["niobium-fhetch-src"]["locked"]

if locked.get("rev") == rev and "narHash" in locked:
    print("flake.lock already in sync")
    sys.exit(0)

locked["rev"] = rev
locked.pop("narHash", None)
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
print("flake.lock updated (narHash cleared; nix will recompute on next invocation)")
PYEOF
