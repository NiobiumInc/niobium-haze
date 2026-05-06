#!/usr/bin/env bash
# Launch clangd with the driver from compile_commands.json on its
# query-driver allow-list, resolved at spawn time.
#
# clangd refuses to introspect arbitrary drivers without --query-driver.
# We refuse to ship a /nix/store glob in checked-in config (any pattern
# can match an unrelated derivation in the store). Resolving from the
# compile DB on every spawn yields one literal absolute path that
# matches the driver clangd will actually invoke for each TU, even
# across flake bumps where the devshell's clang++ has rotated ahead of
# the last `make build`. Falls back to the devshell's clang++ when no
# compile DB is present yet (first-time bootstrap).
#
# Invoke from inside `nix develop` or via `direnv exec . scripts/zed-clangd.sh`.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)

# Prefer the debug build tree for editor diagnostics; fall back to
# release. Default to dbuild/ when neither exists yet — debug is the
# Makefile default, so that's where the next `make build` will land.
if [[ -f "$project_root/dbuild/compile_commands.json" ]]; then
    db_dir="dbuild"
elif [[ -f "$project_root/build/compile_commands.json" ]]; then
    db_dir="build"
else
    db_dir="dbuild"
fi
db="$project_root/$db_dir/compile_commands.json"

driver=""
if [[ -f "$db" ]]; then
    # First "command" entry's first whitespace-delimited token is the
    # compiler path. Single-awk-pass — a sed|head pipeline here trips
    # SIGPIPE under `set -o pipefail` and aborts before we exec clangd.
    driver=$(awk -F'"' '/"command": "/{split($4, a, " "); print a[1]; exit}' "$db")
fi

if [[ -z "$driver" || ! -x "$driver" ]]; then
    fallback=$(command -v clang++ || true)
    if [[ -z "$fallback" ]]; then
        echo "zed-clangd: clang++ not on PATH and no compile DB at $db — enter the haze devshell and run 'make build'" >&2
        exit 1
    fi
    driver=$(readlink -f "$fallback")
fi

clangd_bin=$(command -v clangd || true)
if [[ -z "$clangd_bin" ]]; then
    echo "zed-clangd: clangd not on PATH; enter the haze devshell first" >&2
    exit 1
fi

exec "$clangd_bin" "--query-driver=$driver" "--compile-commands-dir=$db_dir" "$@"
