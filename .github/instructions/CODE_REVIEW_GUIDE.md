# Code Review Guide — libhaze

This guide defines how automated and manual PR reviews should be conducted for the `niobium-haze` repository. Reviews must be actionable, concise, and focused on correctness and safety.

> **Scope reminder**: This is a *runtime shim* — not an FHE application or compiler. FHE circuit correctness is out of scope here. The critical areas are the record-and-replay contract, shadow storage invariants, lock ordering, and the C ABI boundary.

> **Automated review scope**: Automated reviews perform static file analysis only (Read, Write, Edit tools). Build validation (`make test`, `make test-sim`), sanitizer runs, and transport tests require manual review or dedicated CI workflows.

---

## Project Context

- **What it is**: A CUDA-shaped record-and-replay runtime for the Niobium FHE accelerator (`libhaze`). The public C ABI (`include/haze/haze.h`) mirrors CUDA function shapes one-for-one so CUDA-targeting code ports by mechanical prefix substitution.
- **Stack**: C++23, Clang 19+, CMake ≥ 3.22. `-Wthread-safety` (TSA) is the canonical enforcement path for lock contracts.
- **Key subsystems**:
  - `src/api/` — thin `extern "C"` shims, argument validation, `hazeError_t` translation.
  - `src/core/` — `EpochState`, `DeviceAllocator`, `CompilerBackend`, `Config`.
  - `src/common/` — `HazeMutex`, `HazeLockGuard`, error/log utilities.
  - `replay_bridge/` — OpenFHE integration, `cryptocontext.dat`, ciphertext template generation.
  - `include/haze/` — public C ABI (no C++ in interface).
- **Execution model**: Every compute call appends a node to the FHETCH trace. Outputs are **explicit**: `hazeTagOutput(ptr)` declares a result an output, then `hazeFlush()` is the **flush trigger** — it calls `replay_and_populate()`, dispatches the trace, and writes simulator results into the tagged outputs' shadow buffers. `hazeMemcpy(D2H)` is then a **pure shadow read**; a D2H of an address that was not tagged-and-flushed returns `HAZE_ERROR_NOT_FLUSHED`. `hazeDeviceSynchronize` and `hazeStreamSynchronize` are no-ops (nothing runs asynchronously, so there is nothing to wait for — they do not flush).

---

## 1. Required Review Output

Every review must produce these sections in order:

1. **Summary** (2–4 lines): what changed and why, as inferred from the diff.
2. **Blockers** (must-fix): correctness bugs, ABI breaks, lock order violations, flush-trigger violations, exceptions crossing the C boundary.
3. **Risks / Watch-outs**: areas likely to regress silently — shadow storage contract, thread safety, hazeDeviceReset + bridge re-init pattern.
4. **Non-blocking suggestions**: code duplication, unnecessary complexity, consistency with surrounding code.
5. **Questions for the author**: missing context, unclear assumptions.

Label every issue: **Blocker / High / Medium / Low**.

---

## 2. PR Risk Classification

### Blocker — must not merge
- **Lock order violation**: any path where `DeviceAllocator` calls back into `EpochState` while holding `EpochState::mutex_`. The permitted direction is epoch → allocator only; the reverse deadlocks.
- **Flush-trigger bypass**: materialization (reading simulator results, writing to shadow storage) must go through `hazeFlush()` → `replay_and_populate()` (or the write-only `hazeWriteProgram()` finalize path). A new `replay_and_populate()` call site — or making `hazeMemcpy(D2H)` flush again instead of being a pure shadow read — breaks the explicit-flush contract.
- **Exception crossing the C ABI boundary**: any `extern "C"` function that can throw. Every public entry point must be `HAZE_NOEXCEPT`.
- **ABI break in `include/haze/`**: removing or reordering public API symbols, changing `hazeError_t` enumerator values, changing struct layouts in `haze_types.h`.
- **`DevAddr` arithmetic outside the C ABI edge**: `DevAddr` is an `enum class : uintptr_t`; it must only be cast to/from `void*` at `src/api/` entry points, never inside core or common.
- **Undefined behavior, use-after-free, or memory corruption** in any path.
- **Missing `HAZE_API` on new public symbols** or `HAZE_NOEXCEPT` on new public entry points.
- **Raw owning pointers or manual `new`/`delete`** outside of C-API wrappers. libhaze targets C++23 — ownership must be expressed via `std::unique_ptr`, `std::shared_ptr`, or RAII value types. A raw owning pointer is never acceptable in new code.

### High Risk — deep review required
- Changes to `src/core/epoch.cpp` (recording state machine, `replay_and_populate`, `EpochSession` RAII guard).
- Changes to `src/core/allocator.*` (shadow storage, `alloc_set_`, `shadow_data_`, `extract_polynomial_components`).
- Changes to `include/haze/haze.h` or `include/haze/haze_types.h` (public ABI).
- Changes to `replay_bridge/` (OpenFHE integration, key extraction, `post_recording_hook` registration).
- Any new or modified use of `fhetch::sr_*`, `tag_input`, `tag_output`, `fhetch::result` (FHETCH recording surface).
- Changes to `HazeMutex` / `HazeLockGuard` or any mutation to lock annotations (`HAZE_REQUIRES`, `HAZE_EXCLUDES`).
- Changes to `src/core/config.hpp` (especially `kLocalTarget` or `kHbmBase`).
- Multi-residue (MRP) paths: `hazeModUp`, `hazeModDown`, `hazeMemcpyMrp`, and any `.template` file generation.
- Any fallible internal function that does not return `std::expected<T, HazeInternalError>` — mismatched error propagation hides failures at the API edge.

### Medium Risk — targeted review
- New compute API implementations in `src/api/compute.cpp`.
- New or modified test cases under `test/`.
- Changes to `hazeConfigureDevice`, `hazeMalloc`, `hazeFree` (single-size invariant enforcement).
- Changes to the `EpochSession` RAII guard lifecycle (constructor, destructor, `ensure_initialized`).

### Low Risk — light review
- Docs, comments, README updates.
- Small isolated refactors with no behavior change.
- `test/e2e/` additions that follow the canonical `ops.hpp` pattern.

---

## 3. Core Correctness Checklist

### 3.1 C++23 Ownership, RAII, and `std::expected`
libhaze is authored with Rust-informed preferences: strong ownership, exhaustive RAII, and `std::expected` as the standard fallible return type. Flag any deviation.

- **`std::expected<T, HazeInternalError>`**: every internal function that can fail must return this type. Output parameters, error codes returned via reference, or bool+side-channel patterns are not acceptable in new code. If an existing function uses a legacy pattern and the PR touches it, flag it as a Medium to migrate.
- **RAII everywhere**: every resource with a non-trivial destructor (file handles, `niobium::compiler()` scopes, mutex guards, any OS resource) must be managed by a wrapping value type or `std::unique_ptr`. Manual cleanup in destructors or `goto`-style cleanup blocks are Blockers.
- **No raw owning pointers**: `T*` is only acceptable as a non-owning borrow (e.g., passing a pointer into a C API). Any `T*` that owns its pointee and isn't immediately wrapped in a smart pointer is a Blocker.
- **Value types over pointer soup**: prefer `std::optional<T>` over nullable pointers for optional values; prefer returning `std::expected` over passing a success-flag output parameter.
- **`[[nodiscard]]` on `std::expected` returns**: callers that discard a fallible result silently swallow errors. Flag unchecked `std::expected` return values in any non-test code.
- **Move semantics**: large types (e.g., shadow buffers, polynomial handles) should be moved, not copied, across ownership boundaries. Unnecessary copies in hot paths are a Medium issue.

### 3.2 Lock Order and Thread Safety
This is the most subtle correctness area in libhaze.

- **Lock order**: `EpochState::mutex_` → `DeviceAllocator`. Never the reverse. Any new call from allocator-side code into `EpochState` is a Blocker.
- **TSA annotations**: every new lock usage must carry `HAZE_REQUIRES` or `HAZE_EXCLUDES` on the calling function. Missing annotations defeat clang's `-Wthread-safety` enforcement.
- **`HazeMutex` / `HazeLockGuard`**: use these, not `std::mutex` / `std::lock_guard`. The standard library types carry no TSA annotations.
- **Shared data without synchronization**: flag any access to `EpochState` or `DeviceAllocator` fields from outside `mutex_`-protected regions.
- **`CompilerBackend::init`**: called once under lock-free fast path. Any new initialization that is not idempotent or not thread-safe is a Blocker.

### 3.3 Shadow Storage Contract
- **`alloc_set_` vs `shadow_data_`**: `alloc_set_` tracks lifetime (`hazeMalloc`/`hazeFree`); `shadow_data_` tracks payload (exists only after H2D/memset/D2D/`update_shadow`). Code that reads `shadow_data_` without first checking `alloc_set_` membership is a correctness bug.
- **Missing entry reads**: D2H from an address with no `shadow_data_` entry must return `OutputNotFlushed` (`HAZE_ERROR_NOT_FLUSHED`) — nothing was tagged-and-flushed there, so there is nothing to read (a plain H2D'd input still has a `shadow_data_` entry and reads back fine). Compute / D2D extract from a missing entry must return `SourceUnavailable` — using an uninitialized device address is a contract violation, not a defensive case.
- **`extract_polynomial_components`**: this evicts the shadow entry. Any code that reads the shadow after calling this is a use-after-evict bug.
- **`hazeFree` eviction**: shadow entries at freed addresses must be cleared; leaking them past `hazeFree` causes ghost data in a subsequent `hazeMalloc` at the same address.

### 3.4 Record-and-Replay State Machine
- **Explicit output contract**: the flow is compute → `hazeTagOutput(ptr)` → `hazeFlush()` → `hazeMemcpy(D2H)`. `hazeTagOutput` declares an output (tagging any MRP residue promotes its whole group); `hazeFlush` executes the recording and materializes **only** the tagged outputs; D2H is a pure shadow read. Output-hood is the caller's declared intent — `store_compute_result_locked` must not auto-tag every computed value.
- **Flush is the sole trigger**: `replay_and_populate()` may only be reached via `hazeFlush()` (and the write-only `hazeWriteProgram()` finalize path). `hazeMemcpy(D2H)` must NOT flush, and no other site may call it. Any new call site is a design violation.
- **`EpochSession` RAII guard**: every compute call must go through an `EpochSession`. Missing it skips `ensure_initialized` and the recording start/stop lifecycle. `hazeTagOutput` deliberately does NOT use it — tagging is a declaration, not a compute op, and must not start a phantom recording.
- **`tag_input` promotion**: `epoch().lookup_or_create_locked(addr)` creates a fresh `fhetch::Polynomial` from shadow bytes. Calling this on an address whose shadow was already evicted via `extract_polynomial_components` is a silent data hazard.
- **`tag_output` registration**: only explicitly-tagged outputs (`pending_outputs_` and `pending_mrp_groups_`) are tagged for `fhetch::tag_output` before `stop_epoch()`. An untagged computed value is never materialized; a later D2H of it returns `HAZE_ERROR_NOT_FLUSHED`, not stale zeros.
- **No-op when idle**: `replay_and_populate()` / `hazeFlush()` must be a no-op when no FHETCH epoch is in flight or no outputs were tagged (plain H2D-then-D2H round-trips, double-flush). Confirm the guard condition is preserved.

### 3.5 C ABI Boundary
- **`HAZE_NOEXCEPT`**: all `extern "C"` entry points must be `HAZE_NOEXCEPT`. Check every new or modified function in `src/api/`.
- **`HAZE_API` visibility**: new public symbols need `HAZE_API`; implementation-internal symbols must not have it.
- **`hazeError_t` translation**: `std::expected` errors from core must be translated to `hazeError_t` at the API edge, not propagated as exceptions or left untranslated.
- **No C++ types in public headers**: `include/haze/haze.h` and `haze_types.h` must stay pure C. Any new `struct`, `enum`, or function signature must be valid in C99.
- **`DevAddr` cast sites**: the only legitimate `(DevAddr)ptr` and `(void*)addr` casts are at `src/api/` entry points. Flag any cast in `src/core/` or `src/common/`.

### 3.6 Single-Size Allocation Invariant
- Every `hazeMalloc` allocation must equal `ring_dim * sizeof(uint64_t)`. Any path that accepts a different size or skips the size check is a Blocker.
- `hazeConfigureDevice` must be called before the first `hazeMalloc`. New code that calls `hazeMalloc` without a prior configure is a contract violation.
- `hazeHostAlloc` is the correct path for non-polynomial scratch (pointer arrays, twiddle tables, kernel-arg packs) — these must never go through `hazeMalloc`.

### 3.7 hazeDeviceReset and Bridge Re-init
- Any test that calls `hazeDeviceReset` must re-call `hazeReplayBridgeInitCryptoContext` before any subsequent MRP compute call. The `post_recording_hook` is wiped by reset.
- The canonical helper is `integration_helpers.hpp::setup_integration_compute_config`. New test cases that bypass it and reset manually must replicate this re-init.
- Missing re-init after reset causes a null hook dereference on the next `stop_epoch` that triggers `post_recording_hook`.

### 3.8 Code and Data Structure Reuse
- Does new code solve a problem already handled by an existing helper in `src/common/` or `src/core/`? Search before flagging.
- Are there two near-identical implementations of the same operation (e.g., shadow read/write in two places)? Flag and suggest consolidation.
- Is a new type introduced that duplicates the shape of `DevAddr`, `HazeInternalError`, or an existing handle type?

### 3.9 Build System
- Are new source files added to `CMakeLists.txt`?
- Are new symbols in `include/haze/` exported correctly (`HAZE_API`, visibility)?
- Are new dependencies introduced? They must be available in the nix flake and documented.
- Does the change affect `replay_bridge/` linkage? That target must stay isolated so OpenFHE includes do not leak into `libhaze` sources.

---

## 4. Comment Style

- **Blocker**: `[Blocker] file:line — problem — fix`
- **High**: `[High] file:line — problem — suggested fix`
- **Medium / Low**: one line is enough unless a fix is non-obvious.
- Prefer proposing a minimal patch over describing the problem abstractly.
- Do not flag style issues unless they affect readability or create ambiguity.
