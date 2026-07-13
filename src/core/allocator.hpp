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
// Test peer (Attorney pattern); defined under test/, never linked into production.
class AllocatorTestAccess;
} // namespace test

// Single-size device allocator with sparse-map shadow storage: every allocation
// is one single-residue polynomial of poly_bytes_ (N × uint64_t); non-polynomial
// scratch (pointer arrays, twiddle tables, kernel-arg packs) belongs in host
// memory or hazeHostAlloc, not here.
//
// Storage model: alloc_set_ tracks live DevAddrs (size implicit, == poly_bytes_);
// shadow_data_ is a sparse DevAddr -> vector<uint64_t> map holding components only
// for addresses currently carrying user-written or materialized data (most
// intermediates live entirely in fhetch::Polynomial storage and need no shadow).
// An entry-less read returns OutputNotFlushed (D2H) or NoData (compute extract);
// H2D/memset/update_shadow create it; compute extract, a compute-result/D2D re-bind
// (via evict_shadow), and hazeFree evict it. Byte-granular C-ABI access
// reinterpret_casts the over-aligned vector<uint64_t> storage to uint8_t*.
//
// Contract: Config::set_ring_dimension before the first allocate (poly size =
// N × 8 bytes); every allocation must equal that size exactly (else
// PolySizeMismatch); hazeMalloc reserves a DevAddr without zero-init or byte
// storage, and the first write creates the shadow entry.
//
// Contract:
//  - `hazeConfigureDevice` must be called before the first `allocate`.
//    The polynomial size is derived from N (= N × 8 bytes).
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
    // Set the pool's polynomial size; a size change drains the pool, size 0
    // disables pooling.
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

    // Batched allocate for an MRP group under a single lock acquisition (not once
    // per residue); each poly must equal the configured size, a partial failure
    // rolls back everything reserved here, and count == 0 yields an empty result.
    std::expected<std::vector<DevAddr>, HazeInternalError> allocate_many(size_t count,
                                                                         size_t bytes) noexcept
        HAZE_EXCLUDES(mutex_);

    // Frees every address (null entries skipped per hazeFree(nullptr)) and returns
    // the first error encountered.
    std::expected<void, HazeInternalError> free_many(std::span<const DevAddr> addrs) noexcept
        HAZE_EXCLUDES(mutex_);

    // Bulk operations on one allocation; each validates count against the size.
    std::expected<void, HazeInternalError> copy_h2d(DevAddr dst, const void *src,
                                                    size_t count) noexcept HAZE_EXCLUDES(mutex_);
    std::expected<void, HazeInternalError> memset(DevAddr addr, int value, size_t count) noexcept
        HAZE_EXCLUDES(mutex_);

    // Snapshot the shadow buffer as ring_dim uint64_t coefficients for promotion to
    // a FHETCH input; returns NoData (surfaced as SourceUnavailable) when the address
    // has no shadow, and evicts the entry on success (hence non-const).
    HAZE_API std::expected<std::vector<uint64_t>, HazeInternalError>
    extract_polynomial_components(DevAddr addr, uint64_t ring_dim) noexcept HAZE_EXCLUDES(mutex_);

    // Non-evicting copy of the shadow components, for the H2D eager-tag path that
    // must leave shadow_data_ intact for a later compute-free D2H.
    HAZE_API std::expected<std::vector<uint64_t>, HazeInternalError>
    read_polynomial_components(DevAddr addr, uint64_t ring_dim) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Read shadow bytes into a host buffer (shadow only; caller handles any
    // compute materialization).
    std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src,
                                                        size_t count) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Full-replace write of the shadow buffer; `values` is truncated
    // or zero-padded to `poly_bytes_ / sizeof(uint64_t)` then moved in.
    std::expected<void, HazeInternalError> update_shadow(DevAddr addr,
                                                         std::vector<uint64_t> &&values) noexcept
        HAZE_EXCLUDES(mutex_);

    // Pointer-attribute query: DEVICE for hazeMalloc pointers, HOST for
    // hazeHostAlloc pointers, UNREGISTERED otherwise (matches
    // cudaPointerGetAttributes from CUDA 11 onward).
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

    // Helpers (caller holds mutex_): validate_poly_size_locked enforces the
    // ring-dim-set + single-size contract; allocate_one_locked / free_one_locked
    // are the single-poly bodies shared by the scalar and batched entry points.
    void clear_pool_locked() noexcept HAZE_REQUIRES(mutex_);
    std::expected<void, HazeInternalError> validate_poly_size_locked(size_t bytes) const noexcept
        HAZE_REQUIRES(mutex_);
    std::expected<DevAddr, HazeInternalError> allocate_one_locked() noexcept HAZE_REQUIRES(mutex_);
    std::expected<void, HazeInternalError> free_one_locked(DevAddr addr) noexcept
        HAZE_REQUIRES(mutex_);

    // mutex_ protects all state below. Lock order across HAZE (full DAG
    // in common/thread_safety.hpp): this mutex is a LEAF — callers
    // already holding EpochState::mutex_ may re-enter here (config sets the
    // polynomial size during single-threaded setup), and allocator-side code
    // must NOT call into any other component while holding this lock.
    mutable HazeMutex mutex_;
    // Live DevAddrs; size is implicit (== poly_bytes_) under the single-size invariant.
    std::unordered_set<DevAddr> alloc_set_ HAZE_GUARDED_BY(mutex_);
    // Sparse uint64_t component storage (vector sized poly_bytes_/sizeof(uint64_t));
    // created by H2D/memset/update_shadow, evicted by
    // extract/evict_shadow/hazeFree/set_polynomial_size/reset.
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
