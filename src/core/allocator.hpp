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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

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
struct Allocation {
    DevAddr addr{};      ///< Virtual device address handed to the user.
    size_t size = 0;     ///< Allocation size in bytes (== poly_bytes_ under strict contract).
    bool pooled = false; ///< True iff this allocation participates in pool recycling.
};

// Single-size device allocator with sparse-map shadow storage.
//
// HAZE's device address space is FHETCH-addressable storage — every
// allocation represents one single-residue polynomial of N × uint64_t
// bytes. Non-polynomial scratch (pointer arrays, twiddle tables, kernel
// arg packs, etc.) is not a HAZE concern; consumers should use ordinary
// host allocations (or `hazeHostMalloc` when page-aligned host memory
// is needed).
//
// Storage model. Each DevAddr has metadata in `map_` (size, pool flag).
// Component payloads live in a separate sparse map `shadow_data_`
// keyed by DevAddr, with each entry a `vector<uint64_t>` sized to the
// allocation. An entry exists only when the address currently carries
// user-written or materialized components; addresses without an entry
// hold zero shadow memory. Reads of an entry-less address return zero
// (D2H) or NoData (compute extract). Writes (H2D / memset / D2D /
// update_shadow) create or overwrite the entry; consumption by
// compute (extract_polynomial_components) evicts it. Byte-granular
// reads/writes from the C ABI go through `reinterpret_cast<uint8_t*>`
// over the vector's storage — well-defined; `vector<uint64_t>::data()`
// is over-aligned for `uint8_t*`.
//
// Contract:
//  - `Config::set_ring_dimension` must be called before the first
//    `allocate`. The polynomial size is derived from N (= N × 8 bytes).
//  - Every allocation must equal that polynomial size exactly. Any
//    other size returns AllocTooSmall.
//  - `hazeMalloc` reserves a DevAddr but does *not* zero-init or
//    allocate any byte storage. The first write — H2D, memset, D2D,
//    or materialization update — creates the shadow entry.
//
// See docs/lazy_shadow_flake.md for an open intermittent-failure
// investigation: gcc Debug ~70% flake rate, clang ~40%, sanitizers
// clean in single runs. Latent bug exists at baseline (no lazy
// shadows) too; lazy just hits it more often via heap-layout shift.
//
// Thread-safety: all public methods take mutex_ themselves (annotated
// HAZE_EXCLUDES). DeviceAllocator must NOT call back into EpochState
// while holding mutex_ — that violates the epoch -> allocator lock
// order and would deadlock.
class DeviceAllocator {
  public:
    // HAZE_API on this and `extract_polynomial_components` exports the
    // symbols from libhaze's hidden-visibility .so so the test binary
    // can resolve them; encapsulation rests on allocator.hpp living in
    // src/ (private include path), not on the visibility attribute.
    HAZE_API static DeviceAllocator &instance() noexcept;

    // Configure the pool's polynomial size. Calling with a size different
    // from the current configuration drains the pool. Calling with size 0
    // disables pooling entirely.
    void set_polynomial_size(size_t bytes) noexcept HAZE_EXCLUDES(mutex_);
    size_t polynomial_size() const noexcept HAZE_EXCLUDES(mutex_);

    std::expected<DevAddr, HazeInternalError> allocate(size_t bytes) noexcept HAZE_EXCLUDES(mutex_);
    hazeError_t free(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // Bulk operations on a single allocation. Each path validates count
    // against the allocation size and sets has_data on success.
    hazeError_t copy_h2d(DevAddr dst, const void *src, size_t count) noexcept HAZE_EXCLUDES(mutex_);
    hazeError_t copy_d2d(DevAddr dst, DevAddr src, size_t count) noexcept HAZE_EXCLUDES(mutex_);
    hazeError_t memset(DevAddr addr, int value, size_t count) noexcept HAZE_EXCLUDES(mutex_);

    // Snapshot the shadow buffer as a vector of `ring_dim` 64-bit limbs.
    // Used by EpochState::lookup_or_create_locked when promoting fresh
    // shadow data to a FHETCH input polynomial. Returns NoData if the
    // address has no shadow entry — the caller can fall back to
    // fhetch::Polynomial::zeros for unwritten reads. **Evicts the
    // shadow entry on success** — the caller now owns the bytes via
    // fhetch's Polynomial storage; the HAZE-side shadow is freed
    // mid-program. Non-const for that reason.
    HAZE_API std::expected<std::vector<uint64_t>, HazeInternalError>
    extract_polynomial_components(DevAddr addr, uint64_t ring_dim) noexcept HAZE_EXCLUDES(mutex_);

    // Read shadow bytes into a host buffer. Caller is responsible for
    // any compute materialization; this only touches the staging buffer.
    hazeError_t copy_to_host(void *dst, DevAddr src, size_t count) const noexcept
        HAZE_EXCLUDES(mutex_);

    // Full-replace write of the shadow buffer; `values` is truncated
    // or zero-padded to `elems_for_allocation` then moved in.
    std::expected<void, HazeInternalError> update_shadow(DevAddr addr,
                                                         std::vector<uint64_t> &&values) noexcept
        HAZE_EXCLUDES(mutex_);

    // Pointer-attribute query. Reports DEVICE for hazeMalloc-allocated
    // pointers, HOST for hazeHostAlloc-allocated pointers, UNREGISTERED
    // for any other pointer (matches cudaPointerGetAttributes from
    // CUDA 11 onward).
    hazeError_t pointer_attributes(hazePointerAttributes *attrs, const void *ptr) const noexcept
        HAZE_EXCLUDES(mutex_);

    bool is_device_pointer(const void *ptr) const noexcept HAZE_EXCLUDES(mutex_);

    // Track an allocation made by hazeHostAlloc so pointer_attributes
    // can report it as HOST. The set is keyed by raw void* — pointers
    // are unique because posix_memalign returns distinct addresses.
    void register_host_pointer(const void *ptr) noexcept HAZE_EXCLUDES(mutex_);
    void unregister_host_pointer(const void *ptr) noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    DeviceAllocator(const DeviceAllocator &) = delete;
    DeviceAllocator &operator=(const DeviceAllocator &) = delete;

  private:
    DeviceAllocator() = default;

    friend class test::AllocatorTestAccess;

    // Byte→element conversion shared by every shadow write path
    // (copy_h2d, copy_d2d, memset, update_shadow). One place so a new
    // write path can't drift from the formula.
    static constexpr size_t elems_for_allocation(const Allocation &a) noexcept {
        return a.size / sizeof(uint64_t);
    }

    // Helper (caller holds mutex_).
    void clear_pool_locked() noexcept HAZE_REQUIRES(mutex_);

    // mutex_ protects all state below. Lock order across HAZE: callers
    // already holding EpochState::mutex_ may re-enter here (epoch →
    // allocator). Allocator-side code must NOT call into EpochState
    // while holding this lock — the reverse direction is forbidden.
    mutable HazeMutex mutex_;
    // Per-address metadata. Entry exists for the lifetime of the
    // hazeMalloc/hazeFree contract.
    std::unordered_map<DevAddr, Allocation> map_ HAZE_GUARDED_BY(mutex_);
    // Sparse uint64_t component storage, vector sized to
    // `elems_for_allocation`. Created by H2D/memset/D2D/update_shadow,
    // evicted by extract / hazeFree / set_polynomial_size / reset.
    std::unordered_map<DevAddr, std::vector<uint64_t>> shadow_data_ HAZE_GUARDED_BY(mutex_);
    std::vector<DevAddr>
        pool_free_ HAZE_GUARDED_BY(mutex_); // free list for poly_bytes_-sized allocations
    std::unordered_set<const void *>
        host_set_ HAZE_GUARDED_BY(mutex_); // pointers from hazeHostAlloc
    size_t poly_bytes_ HAZE_GUARDED_BY(mutex_) = 0;
    uintptr_t next_addr_ HAZE_GUARDED_BY(mutex_) = kHbmBase;
};

inline DeviceAllocator &allocator() noexcept {
    return DeviceAllocator::instance();
}

} // namespace haze
