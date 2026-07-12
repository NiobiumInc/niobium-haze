# C++ Style Guide

Concise house style for libhaze: a Rust programmer's path to Rust-grade safety in C++23. Pairs with `CLAUDE.md`. Every rule below states what *to do* — follow the stated form.

## Mental model (Rust → C++)

- Owned value (`Box<T>`) → stack object, or `std::unique_ptr<T>` via `std::make_unique`.
- Shared owner (`Rc`/`Arc`) → `std::shared_ptr<T>` via `std::make_shared`, where sharing is real (e.g. `fhetch::Polynomial` copy is a refcount bump, not a deep clone).
- `&T` → `const T&` / `std::span<const T>` / `std::string_view`; `&mut T` → `T&`. Keep raw `T*` at the C-ABI edge.
- `Result<T,E>` → `std::expected<T, HazeInternalError>`; `Option<T>` → `std::optional<T>`.
- `match` → `std::visit` over an `overloaded` lambda set, or an exhaustive `switch` over an `enum class` (as in `to_public_error`).
- Trait bound → C++20 concept constraint. `dyn Trait` → abstract base + virtual where the type is runtime-unknown.
- Types: `Vec`→`std::vector`, `[T;N]`→`std::array`, `&[T]`→`std::span`, `String`/`&str`→`std::string`/`std::string_view`, `HashMap`→`std::unordered_map`, `usize`→`size_t`.
- Treat lifetimes as your responsibility: keep every reference / `span` / `string_view` strictly shorter-lived than its referent, and hand-verify it.

## Error handling

- Internal fallible functions return `std::expected<T, HazeInternalError>` and propagate in the value channel (see `EpochState::lookup_or_create_locked`, `Config::set_ring_dimension`).
- Inspect with `if (!result)` then `result.error()`; compose with `.and_then()` / `.transform()` / `.or_else()` / `.transform_error()` / `value_or(...)`. Read `*result` / `.value()` after confirming `has_value()`.
- On a recoverable failure, call `record_internal_error(variant, context)`, restore any state you touched, then `return std::unexpected(variant)`.
- Surface every error upward to a frame that can act on it.
- Reserve exceptions for unrecoverable conditions: throw by value from a `std::exception`-derived type, throw early at the first detected contract breach, catch by `const&` at a boundary.
- Catch programmer bugs and broken invariants with asserts plus `record_internal_error` diagnostics (stderr under `HAZE_DEBUG=1`), kept distinct from the `expected` runtime path.

## C-ABI boundary

- Declare public entry points in the header as `HAZE_API hazeError_t hazeX(...) HAZE_NOEXCEPT`; define them as `extern "C"` shims and keep every exception inside that frame.
- Keep each shim thin: validate null / zero-length args first and `return set_error(HAZE_ERROR_INVALID_VALUE)`; translate `void*` ↔ `DevAddr` via `to_dev_addr` / `to_void_ptr`; delegate to a `haze::` core function; convert its `std::expected` to `hazeError_t` via `set_internal_result(...)`.
- Map errors through the exhaustive `to_public_error` switch (user-actionable variants → specific codes, internal variants → `HAZE_ERROR_INTERNAL`).
- Record the last failure in thread-local `g_last_error`; `hazeGetLastError()` reads-and-clears it.
- Mark every fallible return `[[nodiscard]]` (`set_error`, `set_internal_result`, `ensure_initialized`); `-Werror` turns a dropped error into a build stop. Add `[[nodiscard("reason")]]` where the why is non-obvious.

## RAII & ownership

- Default to the rule of zero (C.20): hold self-managing members (`std::vector`, `std::string`, `std::unique_ptr`) and let the generated special members be correct by composition.
- When you hand-write or `=delete` one special member, declare all five (rule of five).
- Mark move constructor / move assignment `noexcept` so containers relocate by move; leave moved-from objects valid, destructible, and assignable.
- Acquire every mutex through an RAII guard — `HazeLockGuard` or the `EpochSession` scoped guard, which carry the clang TSA capability annotations `std::lock_guard` omits. `EpochSession` brings up the backend lock-free, acquires `epoch().mutex_`, and releases on destruction.
- Pass borrowed access as views (`std::span`, `std::string_view`, `const T&`); pass smart pointers to participate in lifetime — `unique_ptr<T>` by value to take ownership, `unique_ptr<T>&` to reseat, `const shared_ptr<T>&` to retain a count.
- Let `DevAddr` lifetime follow the explicit `allocate`/`free` contract (membership in `alloc_set_`), since device allocations outlive function scope — the documented exception to scope-bound ownership.
- Release a wrapper-less C resource through a scope guard (`gsl::finally`-style) as a fallback behind a real RAII type.

## Locking (TSA contract)

- Annotate every guarded field `HAZE_GUARDED_BY(mutex_)`; `-Wthread-safety` rejects unguarded access at compile time.
- Mark lock-taking public methods `HAZE_EXCLUDES(mutex_)`; mark `_locked`-suffixed helpers `HAZE_REQUIRES(mutex_)` and call them only inside a guarded scope.
- Hold the lock DAG documented in `src/common/thread_safety.hpp` (epoch → {config, allocator}; config → allocator; backend-init → config): code under `EpochState::mutex_` may re-enter `DeviceAllocator`; keep allocator code self-contained (a leaf) so it resolves device state entirely within the allocator.
- Expose a mutex reference through an accessor annotated `HAZE_RETURN_CAPABILITY(mutex_)`.

## Prefer (modern C++20/23)

- `std::expected` / `std::optional` with monadic ops for fallible and absent values.
- `std::span<const T>` / `std::string_view` for non-owning contiguous and string params (size travels with the data); guard with `empty()` before `front()` / `back()`.
- `enum class : T` with an explicit underlying type for strong, dispatchable handles (`DevAddr : uintptr_t`, `HazeInternalError : std::uint8_t`).
- `inline constexpr` for compile-time constants (`kHbmBase`, `kLocalTarget`); `constexpr` / `consteval` + `if consteval` for compile-time evaluation.
- Designated initializers in declaration order (`Type{.field = value}`) for aggregates.
- Return results by value; bundle multiple outputs in a struct or `std::tuple` and unpack with structured bindings.
- `std::print` / `std::println` with a compile-time-constant format string.
- Ranges algorithms and views (`views::transform`, `views::filter`, `ranges::to`) for sequence work.
- `deducing this` (`template<class Self> auto f(this Self&& self)`) to fuse cv-ref overload sets and write recursive lambdas.

## Safe subset

These are the positive forms of C++'s sharp edges — use the stated construct and the edge stays covered.

- Own heap memory through `std::unique_ptr` / `std::make_unique` (or `std::make_shared` for shared ownership) and let destructors release it.
- Convert with named casts (`static_cast`, `reinterpret_cast`); confine `reinterpret_cast` to the ABI helpers `to_dev_addr` / `to_void_ptr`.
- Brace-initialize (`{}`) so narrowing becomes a compile error; request a deliberate narrowing through an explicit `static_cast`.
- Carry contiguous data as `std::array` / `std::vector` / `std::span`, reaching elements via `.at()` or range-based iteration.
- Initialize every variable where it is declared (brace-init or `auto`).
- Express constants and small logic with `constexpr` / `enum class` / `inline` functions and templates; reserve macros for the sanctioned TSA `HAZE_*` attributes.
- Keep each reference / `span` / `string_view` alive only within its referent's lifetime.

## Templates & concepts

- Constrain every template argument with a concept (`template<MyConcept T>` or `void f(std::integral auto x)`); reach for standard concepts (`std::integral`, `std::same_as`, `std::convertible_to`) where they fit.
- Express constraints as use-patterns in a `requires`-expression (simple, type, compound `{ expr } -> Concept`, nested) carrying real semantics.
- Select overloads by positive constraints of differing specificity and let subsumption pick the most-constrained; write each constraint once.
- Dispatch operations through non-type `template<auto OpFn>` parameters — the established compute-kernel pattern (`binary_pp_op<fhetch::sr_addp>`, `unary_pq_op<...>`, `binary_pp_op_mrp<fhetch::mr_addp>`).
- Prefer monomorphized concept-constrained templates for static dispatch; keep virtual dispatch for heterogeneous / runtime-unknown types.
- Process parameter packs with fold expressions (pick fold direction by associativity; supply an init value or `sizeof...` guard for empty packs) and forward with `std::forward<Args>(args)...`.
- Lean on CTAD where the whole `<...>` list is omitted; supply explicit deduction guides where deduction is ambiguous.
- Name computed types with `using` aliases and constrain at the declaration so failures report "constraints not satisfied" at the call site.

## Naming & layout

- Public C ABI: `haze`-prefixed camelCase functions (`hazeMalloc`); `haze`-prefixed types (`hazeError_t`, `hazeStream_t`); `typedef enum` with `NOLINT` blocks; opaque `struct …_s *` handles.
- Internal C++: PascalCase types (`DeviceAllocator`, `EpochState`); snake_case methods (`set_ring_dimension`); `_locked` suffix for lock-holding helpers; `kPascalCase` for `inline constexpr` constants; trailing-underscore snake_case members (`mutex_`, `shadow_data_`); PascalCase `enum class`; `g_` for thread-local globals (`g_last_error`); `*_op` / `*_op_mrp` for compute templates.
- Reach singletons via static `instance()` plus a namespace-scope inline accessor (`allocator()`, `epoch()`, `config()`).
- Start every header with `#pragma once`.
- Define inline conversions / accessors in headers; keep substantive logic in `.cpp`.
- Layout: `include/haze/` public ABI, `src/api/` extern-C shims, `src/core/` implementation, `src/common/` leaf utilities.
- Order includes local-relative first, then `<system/vendor>`, then public `haze/*.h`; `IncludeBlocks: Regroup`. 4-space indent, attached braces, 100-column limit (in-tree `.clang-format`).

## References

Dedicated "C++ for Rust devs" guides are scarce; the Mental model above is the primary mapping. Cross-language + canonical refs:

- RAII & ownership, C++ vs Rust (The Coded Message): https://www.thecodedmessage.com/posts/raii/
- C++↔Rust type table — Brown "C++ to Rust Phrasebook"; read the C++ column for the Rust idiom you know: https://cel.cs.brown.edu/crp/idioms/type_equivalents.html
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- C++23 feature reference: https://en.cppreference.com/cpp/23
- `std::expected` (C++23): https://www.cppstories.com/2024/expected-cpp23/
- Monadic `std::optional` (C++23): https://www.cppstories.com/2023/monadic-optional-ops-cpp23/
- `std::span` (C++20): https://www.cppstories.com/2023/span-cpp20/
- Concepts vs SFINAE: https://mariusbancila.ro/blog/2019/10/04/concepts-versus-sfinae-based-constraints/
- Deducing this (C++23): https://devblogs.microsoft.com/cppblog/cpp23-deducing-this/
