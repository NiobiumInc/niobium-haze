# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`libhaze` is a CUDA-shaped record-and-replay runtime for the Niobium FHE
accelerator. The public C ABI (`include/haze/haze.h`) mirrors CUDA function
shapes one-for-one (`cudaMalloc` → `hazeMalloc`, `cudaMemcpy` → `hazeMemcpy`,
`cudaStreamSynchronize` → `hazeStreamSynchronize`, etc.) so codebases written
against CUDA — primarily FIDESlib — can port by mechanical prefix substitution.

The implementation does not execute polynomial math. Every compute call records
a node into FHETCH IR via `niobium::fhetch::sr_*`; the next `hazeMemcpy(D2H)`
finalizes the trace, dispatches it to a backend (in-process simulator or HTTP
transport to `nbcc_fhetch_replay`), writes the simulator-computed values into
shadow buffers, and only then copies the shadow bytes to the host destination.
D2H is therefore the sole flush trigger; there is no separate explicit-replay
entry point.

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
with everything pinned (clang, cmake, clang-tools, catch2_3, jujutsu)
plus `MACOSX_DEPLOYMENT_TARGET=14.0` and a nix-pinned `SDKROOT`:

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
(`hazeSetRingDimension`, `hazeMalloc`). Two libc++ ABI versions are
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
forces `MODE=release` so the gates exercise the optimization-level
that ships; local iteration defaults to debug so editor diagnostics,
asserts, and sanitizer turnaround line up with the build that gets
produced.

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
binary directly. Tests `cd` into `$(BUILD_DIR)/runs/` (`dbuild/runs/`
by default, `build/runs/` for release) so `niobium::compiler()`'s
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

Three CI gates must pass before a PR merges. Each one has a local
equivalent — run them before pushing rather than after CI fails.

**Primary pre-push check: the flake.** Run all three lint derivations
through `nix build` so local output matches CI byte-for-byte:

```sh
nix build -L --keep-going \
  .#checks.<sys>.clang-format \
  .#checks.<sys>.clang-tidy \
  .#checks.<sys>.clangd-check
```

Substitute `<sys>` for your host (`aarch64-darwin`, `x86_64-linux`,
`aarch64-linux`). If the sandbox can't fetch the `niobium-fhetch-src`
input over SSH (Lima VMs, isolated runners), point it at the local
checkout:

```sh
nix build -L --keep-going \
  .#checks.<sys>.clang-format \
  .#checks.<sys>.clang-tidy \
  .#checks.<sys>.clangd-check \
  --override-input niobium-fhetch-src \
    "git+file://$PWD/vendor/niobium-fhetch?submodules=1"
```

The local checkout must be at the lockfile's rev (or pass
`--no-write-lock-file` to bypass the lock check). `nix flake check -L
--keep-going` runs the same three plus `unit-tests`, `sim-tests`, `fmt`,
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

2. `clang-tidy + clangd-check` — `clang-tidy --warnings-as-errors='*'`
   over first-party `src/`, `replay_bridge/`, `test/` `.cpp` files,
   plus `clangd --check` with a grep-for-warnings post-pass (clangd
   exits zero on warnings without it). Both ride the configured
   `compile_commands.json` and must report zero warnings. Both share
   their lint definitions with the flake check via scripts in
   `scripts/`. Reproduce locally (after `make build`):

   ```sh
   scripts/clang-tidy.sh                  # default; reads dbuild/
   scripts/clangd-check.sh                # default; reads dbuild/

   # Match CI's release-mode database when chasing CI-only failures:
   make build MODE=release
   BUILD_DIR=build scripts/clang-tidy.sh
   BUILD_DIR=build scripts/clangd-check.sh
   ```

When iterating on a single file, skip the flake build and call the
linters directly — but keep `--warnings-as-errors='*'` so local output
matches what CI promotes to errors:

```sh
clang-tidy -p dbuild --warnings-as-errors='*' src/api/compute.cpp
clangd --check=src/api/compute.cpp
```

The `scripts/clang-tidy.sh` and `scripts/clangd-check.sh` wrappers honor
`BUILD_DIR=<dir>` (default `dbuild`, matching the Makefile's debug
default; pass `BUILD_DIR=build` after `make build MODE=release` to
match CI) and `PARALLEL_JOBS=<n>` (clang-tidy only; defaults to
`NIX_BUILD_CORES` or the host CPU count).

A bare `clang-tidy -p dbuild <file>` (no `--warnings-as-errors`) prints
the same diagnostics as warnings; CI will still fail. Always pass
`--warnings-as-errors='*'` locally when validating before push.

Compiler diagnostics: `-Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion`,
plus `-Wthread-safety` under clang. Clang's TSA is the canonical enforcement
path for the lock contracts in `src/common/thread_safety.hpp`.

## High-level architecture

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

### The lazy record-and-replay execution model

This is the central design contribution. Every compute API call
(`hazeAdd`, `hazeMul`, `hazeNTT`, ...) runs through an `EpochSession` RAII
guard that:

1. Calls `backend().ensure_initialized()` to bring up `niobium::compiler()`
   on first compute (idempotent, lock-free fast path).
2. Acquires `EpochState::mutex_` and starts FHETCH recording if not active.
3. Resolves each `void*` operand: `epoch().lookup_or_create_locked(addr)`
   either returns the polymap binding for a previously-bound DevAddr, or
   promotes the shadow byte buffer at that address to a fresh
   `fhetch::Polynomial` tagged as input.
4. Emits the FHETCH instruction (`fhetch::sr_addp` etc.) and stores the
   result polynomial into the polymap via
   `store_compute_result_locked(dst_addr, poly)`.

No hardware, simulator, or polynomial math runs at this point. The
recording phase only appends nodes to the FHETCH trace.

Materialization is triggered implicitly by `hazeMemcpy(D2H)`. The D2H
path in `haze::copy_to_host` (src/core/epoch.cpp) calls
`EpochState::replay_and_populate()` before reading the shadow buffer:

1. `EpochState::replay_and_populate()` tags every output binding for
   `fhetch::tag_output`. No-op when no recording is in flight, so plain
   H2D-then-D2H round-trips elide it for free.
2. `CompilerBackend::stop_epoch()` writes the per-epoch `.fhetch` trace.
3. `CompilerBackend::replay()` dispatches per the configured target.
4. `niobium::fhetch::result(...)` is called for each tagged output to read
   the simulator-computed polynomial back, and `update_shadow` writes the
   bytes into the allocator's sparse `shadow_data_` map.
5. `copy_to_host` then performs the shadow read into the host buffer.

`hazeDeviceSynchronize` and `hazeStreamSynchronize` are no-ops returning
`HAZE_SUCCESS` — synchronization is implicit in the D2H itself. Streams
and events exist for CUDA-shape parity but do not model ordering.

### Shadow storage model

`DeviceAllocator` keeps two maps keyed by `DevAddr`:

- `map_`: per-allocation metadata (`size`, `pooled`). Lives for the entire
  `hazeMalloc` / `hazeFree` lifetime.
- `shadow_data_`: sparse byte payload. Entry exists only when the address
  carries user-written or materialized bytes. H2D / memset / D2D /
  `update_shadow` create entries; `extract_polynomial_components` (used
  when promoting bytes to a FHETCH input) and `hazeFree` evict them.
  Reads from a missing entry return zero (D2H) or `NoData` (compute
  extract path, falling back to a zero polynomial).

Every `hazeMalloc` allocation must equal the configured polynomial size
(`ring_dim * sizeof(uint64_t)`). `hazeSetRingDimension` is required before
the first `hazeMalloc`. Non-polynomial scratch (pointer arrays, twiddle
tables, kernel-arg packs) goes through `hazeHostAlloc` or ordinary host
malloc, not `hazeMalloc`.

`DevAddr` is an `enum class : uintptr_t` cast from / to `void*` only at
the C ABI boundary. `kHbmBase = 0x4000000000ULL` keeps haze-allocated
addresses above FHETCH's synthetic address range (< `0x1000000000`).

### Lock order

`EpochState::mutex_` may call into `DeviceAllocator` (epoch → allocator).
The reverse direction is forbidden — allocator-side code must never call
back into `EpochState` while holding `mutex_` or it will deadlock. The
constraint is enforced architecturally (no back-call exists in source);
clang TSA catches violations of the per-mutex `HAZE_REQUIRES` /
`HAZE_EXCLUDES` contracts at compile time.

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
runs. The hook is wiped by `hazeDeviceReset`, so any test that resets
must re-call `hazeReplayBridgeInitCryptoContext` afterward — the
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

- `vendor/` is read-only. Public headers under
  `vendor/niobium-fhetch/include` are consumable; modifying source there
  is out of scope. The companion `niobium-fhetch` review goes through
  its own PR.
- C++23, `-Werror`, hidden default visibility on libhaze. `HAZE_API`
  marks public symbols.
- C-ABI public headers use `HAZE_NOEXCEPT`; never throw across the
  boundary. Internal code uses `std::expected<T, HazeInternalError>` for
  fallible paths and translates to `hazeError_t` at the API edge.
- Every test that records must reset state per case
  (`hazeDeviceReset`) and re-init the bridge if it uses MRP outputs.
  See `integration_helpers.hpp::setup_integration_compute_config`.
- Tests `cd` into a runs dir before recording so `program_dir`
  resolves under the build tree, not the source tree.
- For new CKKS-op e2e tests, see `test/e2e/README.md` — the
  `haze::test::ops::*` abstraction (`test/e2e/ops.hpp`) and the bridge
  key-extract APIs (`hazeReplayBridgeExtractEvalMultKey`,
  `…ExtractAutomorphismKey`) cover the canonical patterns.
- `clang-format` / `clang-tidy` configs: in-tree `.clang-format` (LLVM,
  indent 4, column 100) and `.clang-tidy` (see file for the curated
  check list and the three `-Werror`-promoted checks).

