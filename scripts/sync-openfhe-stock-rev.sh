#!/usr/bin/env bash
# Sync the openfhe-stock-src flake input rev in flake.lock to whatever rev the
# vendor/openfhe submodule is checked out at in the haze worktree's index. Run
# after bumping the stock OpenFHE submodule so the two sources of truth stay in
# lockstep; the flake-check CI job fails if they disagree. (The daily
# openfhe-bump workflow keeps them in sync automatically; this is the by-hand
# path.)
#
# Mirrors scripts/sync-fhetch-rev.sh:
# - With nix on PATH, `nix flake lock --override-input` refetches and recomputes
#   every derived field precisely.
# - Without nix, fall back to a python3 JSON edit (set `rev`, drop `narHash`);
#   the next nix invocation refetches and refills narHash. The CI rev-equality
#   gate keys on `rev` only, so the intermediate lock state is enough to merge.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

submodule_rev=$(git ls-tree HEAD vendor/openfhe | awk '{print $3}')
if [[ -z $submodule_rev ]]; then
    echo "error: vendor/openfhe is not registered as a submodule in HEAD" >&2
    exit 1
fi

echo "syncing flake.lock openfhe-stock-src rev -> $submodule_rev"

if command -v nix >/dev/null 2>&1; then
    nix flake lock --override-input openfhe-stock-src \
        "git+https://github.com/openfheorg/openfhe-development.git?rev=${submodule_rev}&submodules=1"
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
locked = data["nodes"]["openfhe-stock-src"]["locked"]

if locked.get("rev") == rev and "narHash" in locked:
    print("flake.lock already in sync")
    sys.exit(0)

locked["rev"] = rev
locked.pop("narHash", None)
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
print("flake.lock updated (narHash cleared; nix will recompute on next invocation)")
PYEOF
