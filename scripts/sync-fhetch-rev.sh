#!/usr/bin/env bash
# Sync the niobium-fhetch-src flake input in flake.lock to the commit
# the vendor/niobium-fhetch submodule is pinned at in the haze
# worktree's HEAD, or, with --check, verify that the two agree. Run it
# after bumping the submodule so the two sources of truth stay in
# lockstep; both flake-check CI jobs run it with --check.
#
# The pin is three fields: `rev`, and that commit's `lastModified`
# (committer time) and `revCount` (number of commits in its history).
# nix rejects a lock whose `lastModified` disagrees with the commit it
# fetches; its shallow git fetch never checks `revCount`, so --check is
# the only check of that field. Both are read from the checked-out
# submodule, which needs its full history for `revCount`.
#
# --check needs only git and python3: it never calls nix, even when nix is
# on PATH.
#
# Syncing has two paths:
#
# - With nix on PATH, defer to `nix flake lock --override-input`. This
#   refetches and recomputes every field, narHash included.
#
# - Without nix, fall back to a python3 JSON edit: set the three pin
#   fields and drop `narHash`. The next nix invocation (e.g. CI's `nix
#   flake check`) refetches and refills `narHash`, so non-nix
#   contributors can update the lock without installing nix locally.
#   python3 is used (instead of jq) because it is more widely
#   pre-installed; its JSON dump with indent=2/sort_keys=True is
#   byte-identical to what nix writes, keeping the diff minimal.
set -euo pipefail

mode=sync
case "${1:-}" in
    --check) mode=check ;;
    "") ;;
    *)
        echo "usage: $(basename "$0") [--check]" >&2
        exit 2
        ;;
esac

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

submodule_rev=$(git ls-tree HEAD vendor/niobium-fhetch | awk '{print $3}')
if [[ -z $submodule_rev ]]; then
    echo "error: vendor/niobium-fhetch is not registered as a submodule in HEAD" >&2
    exit 1
fi

if [[ $mode == sync ]]; then
    echo "syncing flake.lock niobium-fhetch-src rev -> $submodule_rev"
    if command -v nix >/dev/null 2>&1; then
        nix flake lock --override-input niobium-fhetch-src \
            "git+https://github.com/NiobiumInc/niobium-fhetch.git?rev=${submodule_rev}&submodules=1"
        exit 0
    fi
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: need python3 on PATH (or nix, to sync)" >&2
    exit 1
fi

if [[ $(git -C vendor/niobium-fhetch rev-parse --show-toplevel) != "$repo_root/vendor/niobium-fhetch" ]]; then
    echo "error: vendor/niobium-fhetch is not checked out (run make sync)" >&2
    exit 1
fi
if [[ $(git -C vendor/niobium-fhetch rev-parse --is-shallow-repository) == true ]]; then
    echo "error: vendor/niobium-fhetch is a shallow clone; revCount needs its full history (git -C vendor/niobium-fhetch fetch --unshallow)" >&2
    exit 1
fi
last_modified=$(git -C vendor/niobium-fhetch log -1 --format=%ct "$submodule_rev")
rev_count=$(git -C vendor/niobium-fhetch rev-list --count "$submodule_rev")

python3 - "$mode" "$submodule_rev" "$last_modified" "$rev_count" <<'PYEOF'
import json
import pathlib
import sys

mode = sys.argv[1]
pin = {"rev": sys.argv[2], "lastModified": int(sys.argv[3]), "revCount": int(sys.argv[4])}
path = pathlib.Path("flake.lock")
data = json.loads(path.read_text())
locked = data["nodes"]["niobium-fhetch-src"]["locked"]
locked_pin = {field: locked.get(field) for field in pin}

if mode == "check":
    if locked_pin != pin:
        print(
            f"error: flake.lock niobium-fhetch-src pin {locked_pin} does not match "
            f"vendor/niobium-fhetch {pin}; run scripts/sync-fhetch-rev.sh and commit flake.lock",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"flake.lock niobium-fhetch-src pin matches vendor/niobium-fhetch {pin}")
    sys.exit(0)

if locked_pin == pin and "narHash" in locked:
    print("flake.lock already in sync")
    sys.exit(0)

locked.update(pin)
locked.pop("narHash", None)
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
print("flake.lock updated (narHash cleared; nix will recompute on next invocation)")
PYEOF
