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

#include "haze_errors.hpp"
#include "haze_handle.hpp"

#include <haze/haze_types.h>

#include <cstdint>
#include <expected>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze::detail {

// Backing storage for one DevAddr. Data lives in host_shadow until a
// compute call promotes it to a recorded polynomial input.
struct Allocation {
    DevAddr addr{};                   ///< Virtual device address handed to the user.
    size_t size = 0;                  ///< Total allocation size in bytes.
    std::vector<uint8_t> host_shadow; ///< Host staging buffer; same size as `size`.
    bool has_data = false;            ///< True after H2D, D2D, memset, or a flushed compute.
    bool pooled = false;              ///< True iff this allocation participates in pool recycling.
};

// Single-size device allocator.
//
// HAZE's device address space is FHETCH-addressable storage — every
// allocation represents one single-residue polynomial of N × uint64_t
// bytes. Non-polynomial scratch (pointer arrays, twiddle tables, kernel
// arg packs, etc.) is not a HAZE concern; consumers should use ordinary
// host allocations (or `hazeHostMalloc` when page-aligned host memory
// is needed). If a separate off-device allocator turns out to be
// useful, it can be added as its own class — keeping this one focused
// on FHETCH polynomials.
//
// Contract:
//  - `Config::set_ring_dimension` must be called before the first
//    `allocate`. The polynomial size is derived from N (= N × 8 bytes).
//  - Every allocation must equal that polynomial size exactly. Any
//    other size returns AllocTooSmall.
//  - All allocations participate in pool recycling — `hazeFree` pushes
//    the address back to the free list; the next `allocate` pops it
//    and zeroes the shadow.
class DeviceAllocator {
  public:
    static DeviceAllocator &instance() noexcept;

    // Configure the pool's polynomial size. Calling with a size different
    // from the current configuration drains the pool. Calling with size 0
    // disables pooling entirely.
    void set_polynomial_size(size_t bytes) noexcept;
    size_t polynomial_size() const noexcept;

    std::expected<DevAddr, HazeInternalError> allocate(size_t bytes) noexcept;
    hazeError_t free(DevAddr addr) noexcept;

    // Bulk operations on a single allocation. Each path validates count
    // against the allocation size and sets has_data on success.
    hazeError_t copy_h2d(DevAddr dst, const void *src, size_t count) noexcept;
    hazeError_t copy_d2d(DevAddr dst, DevAddr src, size_t count) noexcept;
    hazeError_t memset(DevAddr addr, int value, size_t count) noexcept;

    // Snapshot the shadow buffer as a vector of `ring_dim` 64-bit limbs.
    // Used by EpochState::lookup_or_create_locked when promoting fresh
    // shadow data to a FHETCH input polynomial. Validates that the
    // address is registered, has data, and is sized for the requested
    // ring dimension. Performs the copy under the allocator's lock so
    // there is no separate-locks race with hazeFree / copy_h2d.
    std::expected<std::vector<uint64_t>, HazeInternalError>
    extract_polynomial_components(DevAddr addr, uint64_t ring_dim) const noexcept;

    // Read shadow bytes into a host buffer. Caller is responsible for
    // any compute materialization; this only touches the staging buffer.
    hazeError_t copy_to_host(void *dst, DevAddr src, size_t count) const noexcept;

    // Write bytes into the shadow buffer from a host vector. Used by
    // the materialization engine to publish replayed compute outputs.
    // Truncates to allocation size; sets has_data on success.
    hazeError_t update_shadow(DevAddr addr, const std::vector<uint8_t> &bytes) noexcept;

    // Pointer-attribute query. Reports DEVICE for hazeMalloc-allocated
    // pointers, HOST for hazeHostAlloc-allocated pointers, UNREGISTERED
    // for any other pointer (matches cudaPointerGetAttributes from
    // CUDA 11 onward).
    hazeError_t pointer_attributes(hazePointerAttributes *attrs,
                                   const void *ptr) const noexcept;

    bool is_device_pointer(const void *ptr) const noexcept;

    // Track an allocation made by hazeHostAlloc so pointer_attributes
    // can report it as HOST. The set is keyed by raw void* — pointers
    // are unique because posix_memalign returns distinct addresses.
    void register_host_pointer(const void *ptr) noexcept;
    void unregister_host_pointer(const void *ptr) noexcept;

    void reset() noexcept;

  private:
    DeviceAllocator() = default;
    DeviceAllocator(const DeviceAllocator &) = delete;
    DeviceAllocator &operator=(const DeviceAllocator &) = delete;

    // Helper (caller holds mutex_).
    void clear_pool_locked() noexcept;

    // mutex_ protects all state below. Lock order across HAZE: callers
    // already holding EpochState::mutex_ may re-enter here (epoch →
    // allocator). Allocator-side code must NOT call into EpochState
    // while holding this lock — the reverse direction is forbidden.
    mutable std::mutex mutex_;
    std::unordered_map<DevAddr, Allocation> map_;
    std::vector<DevAddr> pool_free_;             // free list for poly_bytes_-sized allocations
    std::unordered_set<const void *> host_set_;  // pointers from hazeHostAlloc
    size_t poly_bytes_ = 0;
    uintptr_t next_addr_ = kHbmBase;
};

inline DeviceAllocator &allocator() noexcept { return DeviceAllocator::instance(); }

} // namespace haze::detail
