# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`libhaze` is a CUDA-shaped record-and-replay runtime for the Niobium FHE
accelerator. The public C ABI (`include/haze/haze.h`) mirrors CUDA function
shapes one-for-one (`cudaMalloc` → `hazeMalloc`, `cudaMemcpy` → `hazeMemcpy`,
`cudaStreamSynchronize` → `hazeStreamSynchronize`, etc.) so codebases written
against CUDA — primarily FIDESlib — can port by mechanical prefix substitution.

The implementation does not execute polynomial math. Every compute call records
a node into FHETCH IR via `niobium::fhetch::sr_*`. Outputs are explicit: declare
each result with `hazeTagOutput`, then `hazeFlush` finalizes the trace, dispatches it to a backend
(in-process simulator or HTTP transport to `nbcc_fhetch_replay`), and writes the
simulator-computed values into the tagged outputs' shadow buffers. `hazeMemcpy(D2H)`
is then a pure shadow read; reading an address that was not tagged-and-flushed
returns `HAZE_ERROR_NOT_FLUSHED`.

## Toolchain

Required to build, test, and lint:

- A C++23 compiler. Clang (the version nixpkgs-unstable currently ships) is
  what CI tests against; `-Wthread-safety` (the canonical enforcement path for
  the lock contracts in `src/common/thread_safety.hpp`) only fires under clang.
  Clang 19 is the supported floor.
- CMake >= 3.22.
- Catch2 v3 (`Catch2::Catch2WithMain`), discovered via `find_package`.
- For lint/format: clang-format, clang-tidy, clangd (any version that
  understands C++23). Configs in `.clang-format`, `.clang-tidy`, `.clangd`.

If a tool is missing, prefer acquiring it via the project's nix flake
(below) or via `nix-shell -p <pkg>` / `nix shell nixpkgs#<pkg>`. Avoid
`brew install` / `apt install` of build tools unless that is the only
option for the host; the flake is the source of truth for the versions
the project tests against.

### Preferred path: nix flake

If `nix` is installed, use it. The flake provides a hermetic devshell
with everything pinned (clang via `clangStdenv`, cmake, clang-tools,
catch2_3, jujutsu, nixfmt, clang-tidy-cache):

```sh
cd /path/to/niobium-haze
nix develop                                    # interactive shell
nix develop --command bash -c '<command>'      # one-shot
```

To add a new tool the flake doesn't yet provide, edit
`devShells.default.nativeBuildInputs` in `flake.nix` rather than
installing it globally.

### Bare path: build without nix

The Makefile makes no assumption that nix is present. Install the
toolchain via the host package manager and run `make` directly.

macOS (Homebrew):

```sh
brew install llvm cmake catch2
# Only if the host's default clang is < 19:
brew link --force --overwrite llvm
export CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++
```

Linux (Debian/Ubuntu) — clang-19 is the minimum the project supports; pick a
newer apt package (`clang-20`, `clang-21`, ...) where available to match what
nix-based CI tests with:

```sh
sudo apt install clang-19 cmake catch2 clang-format clang-tidy clangd
```

Then build/test/lint per the next section.

### macOS SDK / ABI mismatch trap

This trap triggers only when **mixing** nix and non-nix builds — for
example, OpenFHE / `libnbfhetch` built from a host shell and haze built
inside `nix develop` (or vice versa). A pure-nix or pure-host workflow
is unaffected.

Symptom: a clean release build links with warnings like `object file ...
was built for newer macOS version (26.0) than being linked (14.0)`, then
segfaults non-deterministically inside calls that should be no-ops
(`hazeContextCreate`, `hazeMalloc`). Two libc++ ABI versions are
colliding in the same process.

Fix: rebuild every dylib in the link graph in the same shell so each
carries the same `LC_BUILD_VERSION minos`. From inside `nix develop`:

```sh
rm -rf build dbuild
EXTERNAL_OPENFHE=1 make build
```

If the warnings persist, wipe and rebuild OpenFHE itself in the same
shell. Verify with `otool -l <dylib> | grep -A3 LC_BUILD_VERSION` —
`minos` must agree across `libhaze.dylib`, `libnbfhetch.dylib`, and every
`libOPENFHE*.dylib`. Any non-trivial test segfault accompanied by these
linker warnings is this trap until proven otherwise; do not bisect
before checking.

## Build, test, lint

Top-level `Makefile` is the standalone entry point. `MODE=debug`
(default) selects `dbuild/`; `MODE=release` selects `build/`. CI
forces `MODE=release` (via `build-test.yml`'s explicit flag and via
the flake derivations, which build `haze` with
`-DCMAKE_BUILD_TYPE=Release`) so the gates exercise the optimization
level that ships; local iteration defaults to debug so editor
diagnostics, asserts, and sanitizer turnaround line up with the
binary that gets produced.

```sh
make sync                      # init vendor/niobium-fhetch (recursive)
make build                     # configure + build (debug; build/ for release)
make test                      # test-unit + test-sim (default)
make test-unit                 # ~[integration] tag, HAZE_TARGET=local
make test-sim                  # [integration] tag, in-process FHETCH simulator
make test-transport NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
                               # [integration] via nbcc_fhetch_replay; opt-in
make test-all                  # everything including transport
make clean                     # build, dbuild, OpenFHE outputs (only if owned)
make help                      # full target list
```

Single test (Catch2 tag or substring filter) — bypass make and call the
binary directly. Tests `cd` into `$(BUILD_DIR)/runs/` (`dbuild/runs/` by
default, `build/runs/` for release) so `niobium::compiler()`'s
`program_dir` resolves under the build tree:

```sh
mkdir -p dbuild/runs && cd dbuild/runs
HAZE_TARGET=local ../haze_tests "[integration]"           # all integration tests
HAZE_TARGET=local ../haze_tests "hazeAdd: pointwise sum"  # one case by name
HAZE_TARGET=local ../haze_tests --list-tests              # enumerate
```

Target dispatch is two-tier: `"local"` runs the in-process FHETCH simulator
end-to-end; any other target string (`FUNC_SIM`, `FHE_SIM`, `FPGA_TRI`,
`fhetch_sim`) is forwarded verbatim to `nbcc_fhetch_replay` over HTTP
transport and requires `NIOBIUM_COMPILER_ROOT` to point at a compiler
checkout with `build/nbcc_fhetch_replay`. Resolution order for the value
itself: explicit `hazeSetTarget()` call > `HAZE_TARGET` env var > `"local"`
default. `kLocalTarget` in `src/core/config.hpp` is the single source of
truth for the local string literal — keep comparison sites going through
that constant.

### Test success criterion

A change is considered green when both `make test-sim` (in-process FHETCH
simulator) and `make test-transport NIOBIUM_COMPILER_ROOT=...` (HTTP transport
to `nbcc_fhetch_replay`) exit 0 with Catch2 reporting no unexpected failures.

Sanitizers are mutually exclusive `cmake` cache options:
`-DHAZE_SANITIZERS=ON` (ASAN+UBSAN) or `-DHAZE_TSAN=ON`. UBSAN's enum check
is disabled (`-fno-sanitize=enum`) so the C-ABI `hazeError_t` survives
forward-compat enum extension across library versions.

Lint and format use the in-tree `.clang-format` (LLVM, indent 4, column 100)
and `.clang-tidy` (broad set with bugprone/cppcoreguidelines/modernize/etc.).

Two CI gates must pass before a PR merges. Each one has a local
equivalent — run them before pushing rather than after CI fails.

**Primary pre-push check: the flake.** Run both lint derivations
through `nix build` so local output matches CI byte-for-byte:

```sh
nix build -L --keep-going \
  .#checks.<sys>.clang-format \
  .#checks.<sys>.clang-tidy
```

Substitute `<sys>` for your host (`aarch64-darwin`, `x86_64-linux`,
`aarch64-linux`). If the sandbox can't fetch the `niobium-fhetch-src`
input over SSH (Lima VMs, isolated runners), point it at the local
checkout:

```sh
nix build -L --keep-going \
  .#checks.<sys>.clang-format \
  .#checks.<sys>.clang-tidy \
  --override-input niobium-fhetch-src \
    "git+file://$PWD/vendor/niobium-fhetch?submodules=1"
```

The local checkout must be at the lockfile's rev (or pass
`--no-write-lock-file` to bypass the lock check). `nix flake check -L
--keep-going` runs the same two plus `unit-tests`, `sim-tests`, `fmt`,
and the devshell build — slowest, but the strongest pre-push signal.

**`cmake --build` is not a substitute** for any of the lint gates. It
enforces only compiler-level `-Werror`, which doesn't fire on tidy-only
categories (`misc-include-cleaner`, `modernize-use-designated-initializers`,
`readability-isolate-declaration`, etc.). clang-format alone isn't
either. New files need the full check most: existing files have history
that filtered out these patterns, fresh ones don't.

**Faster iteration: call the scripts directly.** Each gate has a script
under `scripts/` that the flake check dispatches through, so behavior
matches:

1. `Check clang-format` — runs `clang-format --dry-run -Werror` over
   every first-party `.cpp` / `.hpp` / `.h` / `.c` under `src/`,
   `include/`, `replay_bridge/`, `test/`. The CI gate, the
   `haze-clang-format` flake check, and the local script all dispatch
   through `scripts/clang-format.sh` so behavior matches across all
   three. Reproduce locally:

   ```sh
   # Apply formatting in place to fix anything the gate would reject.
   scripts/clang-format.sh

   # Same dry-run -Werror sweep CI runs (exits non-zero on any diff).
   scripts/clang-format.sh --check
   ```

   The script resolves the repo root via `git rev-parse`, so it works
   from any subdirectory.

2. `clang-tidy` — `clang-tidy --warnings-as-errors='*'` over first-party
   `src/`, `replay_bridge/`, `test/` `.cpp` files. Rides the configured
   `compile_commands.json` and must report zero warnings. Shares its
   lint definition with the flake check via `scripts/clang-tidy.sh`.
   Reproduce locally (after `make build`):

   ```sh
   scripts/clang-tidy.sh                  # default; reads dbuild/

   # Match CI's release-mode database when chasing CI-only failures:
   make build MODE=release
   BUILD_DIR=build scripts/clang-tidy.sh
   ```

When iterating on a single file, skip the flake build and call
clang-tidy directly — but keep `--warnings-as-errors='*'` so local
output matches what CI promotes to errors:

```sh
clang-tidy -p dbuild --warnings-as-errors='*' src/api/compute.cpp
```

The `scripts/clang-tidy.sh` wrapper honors `BUILD_DIR=<dir>` (default
`dbuild`, matching the Makefile's debug default; pass `BUILD_DIR=build`
after `make build MODE=release` to match CI), `PARALLEL_JOBS=<n>`
(defaults to `NIX_BUILD_CORES` or the host CPU count), and `CLANG_TIDY=<bin>`
(the binary to invoke per file; defaults to `clang-tidy`).

### How CI lints faster than `nix build .#checks.<sys>.clang-tidy`

The flake derivation is the local reproducibility anchor, but it
runs uncached on every invocation. CI bypasses it for the merge gate
and routes through [matus-chochlik/ctcache] (`clang-tidy-cache`),
a Python wrapper that hashes the source content + compile command
+ `.clang-tidy` config and returns the previously-recorded verdict
for unchanged TUs. On a typical PR touching <10% of files this turns
the in-derivation lint (~2 min) into a tens-of-seconds step — most of
the residual time is `nix develop` startup + cache restore/save round-
trips, not clang-tidy itself.

`clang-tidy-cache` is packaged from the `ctcache-src` flake input
and shipped in the devshell, so `CLANG_TIDY=clang-tidy-cache
scripts/clang-tidy.sh` works locally too. To set up a local cache:

```sh
nix develop --command bash -c '
  export CTCACHE_DIR="$HOME/.cache/ctcache-haze"
  export CTCACHE_CLANG_TIDY="$(command -v clang-tidy)"
  CLANG_TIDY=clang-tidy-cache scripts/clang-tidy.sh
'
```

CI's `flake-check.yml` does three things the flake derivation
can't (because nix sandboxes are hermetic and ctcache needs a
persistent cache dir):

1. `nix build .#haze-compile-commands` to fetch a configure-only
   derivation that hands back a self-contained
   `compile_commands.json` — including the `-isystem` flags the
   cc-wrapper would inject at runtime, baked in via
   `CMAKE_CXX_FLAGS` so libclang/clang-tidy resolves headers
   without the wrapper.
2. `actions/cache` to persist `~/.ctcache` across runs, keyed by
   `.clang-tidy` + `flake.lock` + `scripts/clang-tidy.sh`. Any of
   those changing invalidates the cache cleanly.
3. `CLANG_TIDY=clang-tidy-cache BUILD_DIR=build scripts/clang-tidy.sh`
   inside `nix develop` so the wrapper sees the real clang-tidy
   binary via `CTCACHE_CLANG_TIDY`.

A `nix build .#checks.<sys>.clang-tidy` run remains valid for local
pre-push validation and produces byte-identical *findings* (not
byte-identical compile commands — CI's database bakes the cc-wrapper's
`-isystem` flags in directly while the derivation gets them via env);
only the latency differs.

[matus-chochlik/ctcache]: https://github.com/matus-chochlik/ctcache

A bare `clang-tidy -p dbuild <file>` (no `--warnings-as-errors`) prints
the same diagnostics as warnings; CI will still fail. Always pass
`--warnings-as-errors='*'` locally when validating before push.

Compiler diagnostics: `-Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion`,
plus `-Wthread-safety` under clang. Clang's TSA is the canonical enforcement
path for the lock contracts in `src/common/thread_safety.hpp`.

### Finishing a task: self-review + local PR review

Before calling a change done (and before pushing), do two reviews on top of the
gates above:

1. **Self-review the diff** for correctness — lock order, the flush contract,
   shadow-storage invariants, and the C ABI boundary (the areas section 3 of
   `.github/instructions/CODE_REVIEW_GUIDE.md` covers).
2. **Run the local PR review**: `scripts/local-pr-review.sh`. It reproduces the
   GitHub "PR - Claude Code Review" (`.github/workflows/pr-claude-code-review.yml`)
   locally — same model, Read-only, same guide — over this branch's changes vs
   `main` (committed and uncommitted), printing findings instead of posting a PR
   comment. Address them now rather than after CI flags them. `--dry-run` lists
   the files without calling `claude`; `BASE=<ref>` reviews against another base.

### Layered structure

```
include/haze/                 Public C ABI (no C++ in interface).
  haze.h                      Function declarations.
  haze_types.h                Opaque handles, hazeError_t, hazeDeviceProp,
                              CRT basis-convert param structs.

src/api/                      One .cpp per haze.h section. Each entry point
                              is a thin extern "C" shim: validate arguments,
                              translate void* ↔ DevAddr, set the thread-local
                              last-error, delegate to core/.

src/core/                     Implementation. Singletons reachable via
                              haze::config(), haze::backend(), haze::epoch(),
                              haze::allocator().

src/common/                   Shared leaf utilities (errors, log, handle,
                              thread_safety).

replay_bridge/                OpenFHE-using helper that synthesizes
                              cryptocontext.dat + per-output
                              ciphertext_templates/<name>.template files.
                              Lives in its own target so OpenFHE includes
                              stay out of libhaze sources. libhaze links
                              the bridge for niobium::fhetch::result(...)
                              symbol resolution.
```

### The deferred-tape execution model

This is the central design contribution. Every compute API call
(`hazeAdd`, `hazeMul`, `hazeNTT`, ...) records onto an append-only tape
(`src/core/graph.hpp`) without calling fhetch at all:

1. `record_prelude(ctx)` only reads the context's FHE parameters — a
   lock-free `ConfigSnapshot` published once by `hazeContextCreate` and
   immutable for the context's lifetime. It does NOT touch the global
   `niobium::compiler()`: nothing is emitted to fhetch until flush, and
   flush re-initializes the engine from scratch anyway, so the record
   path stays per-context and global-free (a compute on one context
   cannot race a flush on another).
2. Each `void*` operand resolves through the lock-free `BindingTable`
   (addr → `ValueId`, one atomic word per allocation slot). First touch
   snapshots the shadow bytes NOW (evicting) and appends an
   `InputSnapshot` node; the value snapshot is eager, only the fhetch
   emission is deferred.
3. The destination binds a fresh `ValueId` (SSA-style: in-place ops are
   safe because operands resolved first) and one `Node` is appended
   whose THUNK performs the fhetch call (`sr_addp`, `mr_mulp`,
   `fast_base_convert`, ...) at flush time. Thunks capture plain values
   only — never fhetch objects (see the discipline note in graph.hpp).
   Output-hood is not inferred — `hazeTagOutput` appends a metadata
   node.

No fhetch call, no hardware, no polynomial math runs at this point.

Materialization is triggered by `hazeFlush` → `lower::finalize()`
(`src/core/lower.cpp`):

1. `graph().seal()` swaps the tape out; `derive()` replays the
   bookkeeping single-threaded over it in tape order — input/output
   naming (`haze_in_N`/`haze_out_N`), MRP group registration + tag
   expansion, the invalidate group-drop walk, H2D rebind-and-untag.
   Its containers deliberately mirror the old eager engine's
   `unordered_map`s: their iteration order is part of `.fhetch`
   byte-identity (see the constraint note in lower.hpp).
2. `compiler().start_epoch()` + recording start, then thunks run in
   tape order, emitting exactly the instruction stream the eager
   engine produced (values evict after their last consumer, so peak
   memory matches the old keep-latest-per-addr polymap).
3. Explicit outputs (and their MRP groups) get `fhetch::tag_output`;
   `CompilerBackend::stop_epoch()` writes the per-epoch `.fhetch`
   trace; `CompilerBackend::replay()` dispatches per the configured
   target.
4. `niobium::fhetch::result(...)` is called for each tagged output to read
   the simulator-computed polynomial back, and `update_shadow` writes the
   bytes into the allocator's sparse `shadow_data_` map.

`scripts/trace-diff.sh` is the conformance harness for this layer:
record-path changes must keep the per-suite trace artifacts
byte-identical (capture a baseline, capture the candidate, compare).

`hazeMemcpy(D2H)` (`haze::copy_to_host`) is then a pure shadow read.
`hazeDeviceSynchronize` and `hazeStreamSynchronize` are no-ops — nothing runs
asynchronously, so there is no device work to wait for; they exist for
CUDA-shape parity but do not flush and do not model ordering.

### Shadow storage model

`DeviceAllocator` keeps two `DevAddr`-keyed structures:

- `alloc_set_`: set membership covering the lifetime of the
  `hazeMalloc` / `hazeFree` contract. Allocation size is implicit
  (always `poly_bytes_`) under the single-size invariant.
- `shadow_data_`: sparse byte payload. Entry exists only when the address
  carries user-written or materialized bytes. H2D / memset / D2D /
  `update_shadow` create entries; `extract_polynomial_components` (used
  when promoting bytes to a FHETCH input) and `hazeFree` evict them.
  Reads from a missing entry return `OutputNotFlushed` (D2H of an untagged /
  unflushed addr) or `SourceUnavailable` (compute / D2D extract path — using an
  addr that was never written is a contract violation).

Every `hazeMalloc` allocation must equal the context's polynomial size
(`ring_dim * sizeof(uint64_t)`, fixed at `hazeContextCreate`).
Non-polynomial scratch (pointer arrays, twiddle tables, kernel-arg
packs) goes through `hazeHostAlloc` or ordinary host malloc, not
`hazeMalloc`.

`DevAddr` is an `enum class : uintptr_t` cast from / to `void*` only at
the C ABI boundary. `kHbmBase = 0x4000000000ULL` keeps haze-allocated
addresses above FHETCH's synthetic address range (< `0x1000000000`).

### Locking

Haze locks form a hierarchy (the full ordered edge set lives in
`src/common/thread_safety.hpp`), with `lower.cpp`'s file-local
`g_lower_mutex` outermost. The record path is shallow: it takes only
`Graph::append`'s internal mutex (a short push) and the
`DeviceAllocator` mutex (leaf; the allocator call completes before the
append). `lower::finalize` serializes flushes against each other by
holding `g_lower_mutex` across the whole flush, and under it momentarily
acquires the other subsystems' locks one at a time (the open-bracket
check, backend bring-up, `seal()`, shadow population). During lowering
proper — thunk execution, output tagging, replay — only `g_lower_mutex`
is held, so fhetch's own internal locks are reached only by the single
flush thread. The `BindingTable` and `LowerCtx` are
deliberately annotation-free (single-word atomics / single-threaded at
flush); everything mutex-guarded carries `HAZE_GUARDED_BY` /
`HAZE_EXCLUDES`, and clang TSA checks the contracts at compile time.
Cross-thread reuse of one device address without user synchronization
is documented-undefined (CUDA-like) — see the contract block above
`hazeFlush` in `include/haze/haze.h`.

`HazeMutex` (a thin annotated wrapper around `std::mutex`) and
`HazeLockGuard` exist because libstdc++'s `std::mutex` and
`std::lock_guard` carry no TSA capability annotations. Use them for any
new locks.

### Compiler / replay handoff

`niobium::compiler()` is the singleton supplied by `libnbfhetch` (linked
in via `vendor/niobium-fhetch`). Haze interacts with it through a small
control surface (`CompilerBackend`):

- `init` / `start_recording` / `start_epoch` / `stop_epoch` / `replay`.
- The recording-side IR (`fhetch::sr_*`, `tag_input`, `tag_output`,
  `result`) is emitted directly from `epoch.cpp`, bypassing the backend
  wrapper.

Recordings land in a per-program directory under the test working
directory (`build/runs/haze/...`). Each functional epoch produces its own
`.fhetch` trace and `serialized_probes/<name>.ct` files. Multi-residue
(MRP) operations like `hazeModUp` / `hazeModDown` produce
`ciphertext_templates/<name>.template` files via the `replay_bridge`'s
`post_recording_hook`, registered when `hazeReplayBridgeInitCryptoContext`
runs. Every flush scrubs and rebinds the process-global fhetch engine
from the flushing context (`LoweringSession::ensure_backend`), restoring
the hook from the bridge's stored state; `hazeDeviceReset` wipes the
stored state too, so any test that resets must re-call
`hazeReplayBridgeInitCryptoContext` afterward — the
`integration_helpers.hpp::setup_integration_compute_config` helper
enforces this.

For the upstream record/replay API surface (`tag_input`, `probe`,
`result`, `start_epoch`, `stop_epoch`, `enable_hollow_mode`,
`replay_epochs`, etc.), see `vendor/niobium-fhetch`'s
`include/niobium/fhetch_api.h` and the niobium-compiler repo's CLAUDE.md.

### Niobium-fhetch resolution

Two-tier lookup, identical between Makefile and CMakeLists.txt:

1. `NIOBIUM_HAZE_FHETCH_DIR` (cache var or env) — external source tree.
   Used by `niobium-client` when it owns its own `niobium-fhetch`
   checkout.
2. `vendor/niobium-fhetch` (submodule) — for standalone builds. Run
   `make sync` to populate.

`OPENFHE_INSTALL_DIR` similarly defaults to
`<fhetch>/vendor/lib/openfhe`; pass `EXTERNAL_OPENFHE=1` plus an explicit
`OPENFHE_INSTALL_DIR=<path>` to skip the OpenFHE build chain when a
parent project (e.g., niobium-client) has already built it.

## Repository conventions

- `style.md` is the C++ house style guide (Rust-developer lens, C++23,
  safe-subset, templates/concepts). Read it before writing or reviewing
  any C++ in this repo; it complements the `.clang-format` / `.clang-tidy`
  configs with the conventions tooling can't enforce.
- `vendor/` is read-only. Public headers under
  `vendor/niobium-fhetch/include` are consumable; modifying source there
  is out of scope. The companion `niobium-fhetch` review goes through
  its own PR.
- C++23, `-Werror`, hidden default visibility on libhaze. `HAZE_API`
  marks public symbols.
- C-ABI public headers use `HAZE_NOEXCEPT`; never throw across the
  boundary. Internal code uses `std::expected<T, HazeInternalError>` for
  fallible paths and translates to `hazeError_t` at the API edge.
- Every test that records gets per-case isolation from a FRESH context
  (`haze::test::recreate_ctx`, which also resets the process globals)
  and re-inits the bridge if it uses MRP outputs. See
  `integration_helpers.hpp::setup_integration_compute_config`.
- Tests `cd` into a runs dir before recording so `program_dir`
  resolves under the build tree, not the source tree.
- For new CKKS-op e2e tests, see `test/e2e/README.md` — the
  `haze::test::ops::*` abstraction (`test/e2e/ops.hpp`) and the test-side
  key-extract helpers (`haze::test::extract_evalmult_key_limbs` /
  `…extract_automorphism_key_limbs` in `test/openfhe_key_extract.hpp`)
  cover the canonical patterns.
- `clang-format` / `clang-tidy` configs: in-tree `.clang-format` (LLVM,
  indent 4, column 100) and `.clang-tidy` (see file for the curated
  check list and the three `-Werror`-promoted checks).
