// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
#pragma once

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

class DeviceState;

namespace test {
// Forward declaration for the test peer (Attorney pattern); definition
// lives under test/ and is never linked into production.
class AllocatorTestAccess;
} // namespace test

// Per-DevAddr metadata. Pure liveness + pool-recycling info — no
// payload. Component storage lives in DeviceAllocator::shadow_data_,
// which is sparse (an entry exists only for addresses that currently
// hold user-written or materialized uint64_t components). Most
// addresses in a typical FHE program are intermediate compute results
// that flow entirely inside fhetch::Polynomial storage and never need
// a HAZE shadow.
// Single-size device allocator with sparse-map shadow storage.
//
// HAZE's device address space is FHETCH-addressable storage — every
// allocation represents one single-residue polynomial of N × uint64_t
// bytes. Non-polynomial scratch (pointer arrays, twiddle tables, kernel
// arg packs, etc.) is not a HAZE concern; consumers should use ordinary
// host allocations (or `hazeHostAlloc` when page-aligned host memory
// is needed).
//
// Storage model. Live DevAddrs are tracked as set membership in
// `alloc_set_` — allocation size is implicit (always `poly_bytes_`).
// Component payloads live in a separate sparse map `shadow_data_`
// keyed by DevAddr, with each entry a `vector<uint64_t>` sized to the
// allocation. An entry exists only when the address currently carries
// user-written or materialized components; addresses without an entry
// hold zero shadow memory. Reads of an entry-less address return
// OutputNotFlushed (D2H) or NoData (compute extract). Writes (H2D /
// memset / update_shadow) create or overwrite the entry; consumption by
// compute (extract_polynomial_components), a compute-result / D2D
// re-bind (EpochState::store_compute_result_locked via evict_shadow),
// and hazeFree evict it. Byte-granular
// reads/writes from the C ABI go through `reinterpret_cast<uint8_t*>`
// over the vector's storage — well-defined; `vector<uint64_t>::data()`
// is over-aligned for `uint8_t*`.
//
// Contract:
//  - `Config::set_ring_dimension` must be called before the first
//    `allocate`. The polynomial size is derived from N (= N × 8 bytes).
//  - Every allocation must equal that polynomial size exactly. Any
//    other size returns PolySizeMismatch.
//  - `hazeMalloc` reserves a DevAddr but does *not* zero-init or
//    allocate any byte storage. The first write — H2D, memset, D2D,
//    or materialization update — creates the shadow entry.
//
// Thread-safety: all public methods take mutex_ themselves (annotated
// HAZE_EXCLUDES). DeviceAllocator must NOT call back into EpochState
// while holding mutex_ — that violates the epoch -> allocator lock
// order and would deadlock.
class DeviceAllocator {
  public:
    // Configure the pool's polynomial size. Calling with a size different
    // from the current configuration drains the pool. Calling with size 0
    // disables pooling entirely.
    void set_polynomial_size(size_t bytes) noexcept HAZE_EXCLUDES(mutex_);
    size_t polynomial_size() const noexcept HAZE_EXCLUDES(mutex_);

    std::expected<DevAddr, HazeInternalError> allocate(size_t bytes) noexcept HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> free(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // UnknownAddress unless `addr` is a live allocation.
    std::expected<void, HazeInternalError> require_allocated(DevAddr addr) const noexcept
        HAZE_EXCLUDES(mutex_);

    // True while any allocation is live (Config freezes ring_dim on it).
    bool has_live_allocations() const noexcept HAZE_EXCLUDES(mutex_);

    // Drop `addr`'s shadow entry if present — stale bytes must not satisfy
    // a pre-flush D2H after a compute/D2D re-bind.
    void evict_shadow(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // Batched allocate/free for an MRP group: the mutex is taken ONCE for
    // all `count` residues (not once per residue). Both reuse the single-poly
    // logic under the lock.
    //
    // allocate_many reserves `count` polynomials of `bytes` each (each must
    // equal the configured polynomial size, same contract as allocate). On
    // any per-residue failure the polys already reserved in this call are
    // freed and the error returned, so nothing leaks; on success the result
    // holds exactly `count` addresses. count == 0 yields an empty result.
    std::expected<std::vector<DevAddr>, HazeInternalError> allocate_many(size_t count,
                                                                         size_t bytes) noexcept
        HAZE_EXCLUDES(mutex_);

    // free_many attempts every address; zero-valued (null) entries are
    // skipped (hazeFree(nullptr) semantics). It frees all of them and returns
    // the first error encountered, if any.
    std::expected<void, HazeInternalError> free_many(std::span<const DevAddr> addrs) noexcept
        HAZE_EXCLUDES(mutex_);

    // Bulk operations on a single allocation. Each path validates count
    // against the allocation size.
    std::expected<void, HazeInternalError> copy_h2d(DevAddr dst, const void *src,
                                                    size_t count) noexcept HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> memset(DevAddr addr, int value, size_t count) noexcept
        HAZE_EXCLUDES(mutex_);

    // Snapshot the shadow buffer as a vector of `ring_dim` 64-bit
    // coefficients (uint64_t words).
    // Used by EpochState::lookup_or_create_locked when promoting fresh
    // shadow data to a FHETCH input polynomial. Returns NoData if the
    // address has no shadow entry; the caller surfaces this as
    // SourceUnavailable. **Evicts the shadow entry on success** — the
    // caller now owns the bytes via fhetch's Polynomial storage; the
    // HAZE-side shadow is freed mid-program. Non-const for that reason.
    HAZE_API std::expected<std::vector<uint64_t>, HazeInternalError>
    extract_polynomial_components(DevAddr addr, uint64_t ring_dim) noexcept HAZE_EXCLUDES(mutex_);

    // Non-evicting copy of the shadow components. Used by the H2D eager-tag
    // path that must leave shadow_data_ intact (a subsequent compute-free
    // D2H still reads the original H2D bytes).
    HAZE_API std::expected<std::vector<uint64_t>, HazeInternalError>
    read_polynomial_components(DevAddr addr, uint64_t ring_dim) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Read shadow bytes into a host buffer. Caller is responsible for
    // any compute materialization; this only touches the shadow buffer.
    std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src,
                                                        size_t count) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Full-replace write of the shadow buffer; `values` is truncated
    // or zero-padded to `poly_bytes_ / sizeof(uint64_t)` then moved in.
    std::expected<void, HazeInternalError> update_shadow(DevAddr addr,
                                                         std::vector<uint64_t> &&values) noexcept
        HAZE_EXCLUDES(mutex_);

    // Pointer-attribute query. Reports DEVICE for hazeMalloc-allocated
    // pointers, HOST for hazeHostAlloc-allocated pointers, UNREGISTERED
    // for any other pointer (matches cudaPointerGetAttributes from
    // CUDA 11 onward).
    std::expected<void, HazeInternalError> pointer_attributes(hazePointerAttributes *attrs,
                                                              const void *ptr) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Track hazeHostAlloc pointers for pointer_attributes; unregister
    // returns whether the pointer was registered so hazeFreeHost can
    // refuse foreign pointers.
    void register_host_pointer(const void *ptr) noexcept HAZE_EXCLUDES(mutex_);
    [[nodiscard]] bool unregister_host_pointer(const void *ptr) noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    DeviceAllocator(const DeviceAllocator &) = delete;
    DeviceAllocator &operator=(const DeviceAllocator &) = delete;

  private:
    friend class DeviceState;
    DeviceAllocator() = default;

    friend class test::AllocatorTestAccess;

    // Helpers (caller holds mutex_). validate_poly_size_locked enforces the
    // ring-dim-set + single-size contract; allocate_one_locked and
    // free_one_locked are the single-poly bodies shared by the scalar and
    // batched entry points so the logic lives in one place.
    void clear_pool_locked() noexcept HAZE_REQUIRES(mutex_);
    std::expected<void, HazeInternalError> validate_poly_size_locked(size_t bytes) const noexcept
        HAZE_REQUIRES(mutex_);
    std::expected<DevAddr, HazeInternalError> allocate_one_locked() noexcept HAZE_REQUIRES(mutex_);
    std::expected<void, HazeInternalError> free_one_locked(DevAddr addr) noexcept
        HAZE_REQUIRES(mutex_);

    // mutex_ protects all state below. Lock order across HAZE (full DAG
    // in common/thread_safety.hpp): this mutex is a LEAF — callers
    // already holding EpochState::mutex_ or Config::mutex_ may re-enter
    // here, and allocator-side code must NOT call into any other
    // component while holding this lock.
    mutable HazeMutex mutex_;
    // Live DevAddrs. Set membership covers the lifetime of the
    // hazeMalloc/hazeFree contract; allocation size is implicit
    // (== poly_bytes_) under the single-size invariant.
    std::unordered_set<DevAddr> alloc_set_ HAZE_GUARDED_BY(mutex_);
    // Sparse uint64_t component storage, vector sized to
    // `poly_bytes_ / sizeof(uint64_t)`. Created by H2D/memset/update_shadow,
    // evicted by extract / evict_shadow / hazeFree / set_polynomial_size / reset.
    std::unordered_map<DevAddr, std::vector<uint64_t>> shadow_data_ HAZE_GUARDED_BY(mutex_);
    std::vector<DevAddr>
        pool_free_ HAZE_GUARDED_BY(mutex_); // free list for poly_bytes_-sized allocations
    std::unordered_set<const void *>
        host_set_ HAZE_GUARDED_BY(mutex_); // pointers from hazeHostAlloc
    size_t poly_bytes_ HAZE_GUARDED_BY(mutex_) = 0;
    uintptr_t next_addr_ HAZE_GUARDED_BY(mutex_) = kHbmBase;
};

// Defined in device_state.cpp.
DeviceAllocator &allocator() noexcept;

} // namespace haze
