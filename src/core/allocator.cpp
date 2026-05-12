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
#include "core/allocator.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"

#include <cstdint>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>
#include <utility>
#include <vector>

namespace haze {

DeviceAllocator &DeviceAllocator::instance() noexcept {
    static DeviceAllocator inst;
    return inst;
}

void DeviceAllocator::clear_pool_locked() noexcept {
    for (DevAddr addr : pool_free_) {
        map_.erase(addr);
        shadow_data_.erase(addr);
    }
    pool_free_.clear();
}

void DeviceAllocator::set_polynomial_size(size_t bytes) noexcept {
    HazeLockGuard lock(mutex_);
    if (poly_bytes_ == bytes)
        return;
    clear_pool_locked();
    poly_bytes_ = bytes;
}

size_t DeviceAllocator::polynomial_size() const noexcept {
    HazeLockGuard lock(mutex_);
    return poly_bytes_;
}

std::expected<DevAddr, HazeInternalError> DeviceAllocator::allocate(size_t bytes) noexcept {
    HazeLockGuard lock(mutex_);

    // Polynomial size must be configured (via Config::set_ring_dimension)
    // before any allocation. HAZE's device space is FHETCH-addressable
    // storage — without a known polynomial size we cannot serve it.
    if (poly_bytes_ == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "DeviceAllocator::allocate: ring_dim unset");
        return std::unexpected(HazeInternalError::NotConfigured);
    }

    // Strict single-size contract: every allocation is one polynomial.
    // Non-polynomial scratch is not a HAZE concern (use host memory or
    // a future off-device allocator). Mismatches surface here so the
    // consumer is forced to either use the right size or a different
    // allocator.
    if (bytes != poly_bytes_) {
        record_internal_error(HazeInternalError::AllocTooSmall,
                              "DeviceAllocator::allocate: size != polynomial bytes");
        return std::unexpected(HazeInternalError::AllocTooSmall);
    }

    // Recycle from the free list when available. The shadow_data_
    // entry was already evicted at hazeFree time; the recycled DevAddr
    // starts in the same "no shadow, no data" state as a fresh one.
    if (!pool_free_.empty()) {
        DevAddr addr = pool_free_.back();
        pool_free_.pop_back();
        if (map_.contains(addr)) {
            return addr;
        }
        // pool_free_ entry without a map_ peer means internal state is
        // corrupt; fail rather than mint a fresh DevAddr on top.
        record_internal_error(HazeInternalError::PoolMapDesync,
                              "DeviceAllocator::allocate (pool_free_ -> map_)");
        return std::unexpected(HazeInternalError::PoolMapDesync);
    }

    // Fresh allocation: bump the address counter by exactly poly_bytes_.
    // No shadow bytes are allocated. shadow_data_ stays empty for this
    // DevAddr until the first H2D / memset / D2D / update_shadow.
    DevAddr addr{next_addr_};
    next_addr_ += poly_bytes_;

    Allocation a{};
    a.addr = addr;
    a.size = poly_bytes_;
    a.pooled = true;
    map_.emplace(addr, a);
    return addr;
}

hazeError_t DeviceAllocator::free(DevAddr addr) noexcept {
    if (to_uintptr(addr) == 0)
        return HAZE_SUCCESS; // hazeFree(nullptr) matches CUDA semantics

    HazeLockGuard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        record_internal_error(HazeInternalError::UnknownAddress, "DeviceAllocator::free");
        return HAZE_ERROR_INVALID_VALUE;
    }
    // Always evict the shadow on free — the bytes (if any) are no
    // longer the user's to read. For pooled addresses the metadata
    // stays in map_ for recycling; for non-pooled it goes too.
    shadow_data_.erase(addr);
    if (it->second.pooled) {
        pool_free_.push_back(addr);
    } else {
        map_.erase(it);
    }
    return HAZE_SUCCESS;
}

std::expected<std::vector<uint64_t>, HazeInternalError>
DeviceAllocator::extract_polynomial_components(DevAddr addr, uint64_t ring_dim) noexcept {
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "DeviceAllocator::extract_polynomial_components: ring_dim == 0");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    HazeLockGuard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        record_internal_error(HazeInternalError::UnknownAddress,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const size_t expected_bytes = ring_dim * sizeof(uint64_t);
    if (it->second.size < expected_bytes) {
        record_internal_error(HazeInternalError::AllocTooSmall,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::AllocTooSmall);
    }
    auto node = shadow_data_.extract(addr);
    if (!node) {
        // Address allocated but never written; caller may fall back to
        // a zero polynomial via fhetch.
        record_internal_error(HazeInternalError::NoData,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::NoData);
    }
    // Detach the shadow's storage so the caller's from_data() can move
    // it straight into a Polynomial; frees the HAZE-side entry
    // mid-program, before hazeFree.
    auto components = std::move(node.mapped());
    if (components.size() != ring_dim) {
        // Invariant break: every write path sizes the shadow to
        // elems_for_allocation, which equals ring_dim under allocate()'s
        // single-poly-size contract. Surface, don't paper over.
        record_internal_error(HazeInternalError::ShadowSizeMismatch,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::ShadowSizeMismatch);
    }
    return components;
}

hazeError_t DeviceAllocator::copy_h2d(DevAddr dst, const void *src, size_t count) noexcept {
    if (src == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    HazeLockGuard lock(mutex_);
    auto it = map_.find(dst);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    // Lazy-create or overwrite the shadow entry. Sized to the full
    // allocation so partial writes leave the tail at zero (matches
    // prior eager-zero semantics for the unwritten tail).
    auto &shadow = shadow_data_[dst];
    const size_t want_elems = elems_for_allocation(it->second);
    if (shadow.size() != want_elems) {
        shadow.assign(want_elems, uint64_t{0});
    }
    std::memcpy(reinterpret_cast<uint8_t *>(shadow.data()), src, count);
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::copy_d2d(DevAddr dst, DevAddr src, size_t count) noexcept {
    HazeLockGuard lock(mutex_);
    auto src_it = map_.find(src);
    auto dst_it = map_.find(dst);
    if (src_it == map_.end() || dst_it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > src_it->second.size || count > dst_it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    auto src_data = shadow_data_.find(src);
    if (src_data == shadow_data_.end()) {
        // D2D from an address with no live shadow → dst inherits the
        // "no shadow" state. Evict whatever dst had (if anything).
        shadow_data_.erase(dst);
        return HAZE_SUCCESS;
    }
    auto &dst_shadow = shadow_data_[dst];
    // Size from the dst allocation, not src; equal today under the
    // single-poly-size invariant in allocate() but flagged for clarity.
    const size_t want_elems = elems_for_allocation(dst_it->second);
    if (dst_shadow.size() != want_elems) {
        dst_shadow.assign(want_elems, uint64_t{0});
    }
    std::memcpy(reinterpret_cast<uint8_t *>(dst_shadow.data()),
                reinterpret_cast<const uint8_t *>(src_data->second.data()), count);
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::memset(DevAddr addr, int value, size_t count) noexcept {
    HazeLockGuard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    auto &shadow = shadow_data_[addr];
    const size_t want_elems = elems_for_allocation(it->second);
    if (shadow.size() != want_elems) {
        shadow.assign(want_elems, uint64_t{0});
    }
    std::memset(reinterpret_cast<uint8_t *>(shadow.data()), value, count);
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::copy_to_host(void *dst, DevAddr src, size_t count) const noexcept {
    if (dst == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    HazeLockGuard lock(mutex_);
    auto it = map_.find(src);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    auto data_it = shadow_data_.find(src);
    if (data_it == shadow_data_.end()) {
        // Address allocated but no bytes ever written or already
        // consumed by compute. Match the prior eager-zero semantic:
        // return zeros, success. Do not erase any state.
        std::memset(dst, 0, count);
        return HAZE_SUCCESS;
    }
    std::memcpy(dst, reinterpret_cast<const uint8_t *>(data_it->second.data()), count);
    return HAZE_SUCCESS;
}

std::expected<void, HazeInternalError>
DeviceAllocator::update_shadow(DevAddr addr, std::vector<uint64_t> &&values) noexcept {
    HazeLockGuard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        record_internal_error(HazeInternalError::UnknownAddress, "DeviceAllocator::update_shadow");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const size_t want_elems = elems_for_allocation(it->second);
    if (values.size() != want_elems) {
        values.resize(want_elems, uint64_t{0});
    }
    shadow_data_.insert_or_assign(addr, std::move(values));
    return {};
}

bool DeviceAllocator::is_device_pointer(const void *ptr) const noexcept {
    if (ptr == nullptr)
        return false;
    HazeLockGuard lock(mutex_);
    return map_.contains(to_dev_addr(ptr));
}

hazeError_t DeviceAllocator::pointer_attributes(hazePointerAttributes *attrs,
                                                const void *ptr) const noexcept {
    if (attrs == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    *attrs = {};
    // Resolve classification under one locked snapshot, then fill the
    // attribute fields outside the lock. Snapshotting closes the window
    // where a concurrent free() / unregister between membership check
    // and attribute write could flip the answer.
    enum class Classification : uint8_t { Unknown, Device, Host };
    auto classification = Classification::Unknown;
    if (ptr != nullptr) {
        HazeLockGuard lock(mutex_);
        if (map_.contains(to_dev_addr(ptr))) {
            classification = Classification::Device;
        } else if (host_set_.contains(ptr)) {
            classification = Classification::Host;
        }
    }
    switch (classification) {
    case Classification::Device:
        attrs->type = HAZE_MEMORY_TYPE_DEVICE;
        // hazePointerAttributes mirrors cudaPointerAttributes: the
        // devicePointer / hostPointer fields are non-const void*, but
        // the input here is const void*. The cast is contained to the
        // two assignments at the C-ABI boundary.
        attrs->devicePointer =
            const_cast<void *>(ptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        break;
    case Classification::Host:
        attrs->type = HAZE_MEMORY_TYPE_HOST;
        attrs->hostPointer =
            const_cast<void *>(ptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        break;
    case Classification::Unknown:
        attrs->type = HAZE_MEMORY_TYPE_UNREGISTERED;
        break;
    }
    return HAZE_SUCCESS;
}

void DeviceAllocator::register_host_pointer(const void *ptr) noexcept {
    if (ptr == nullptr)
        return;
    HazeLockGuard lock(mutex_);
    host_set_.insert(ptr);
}

void DeviceAllocator::unregister_host_pointer(const void *ptr) noexcept {
    if (ptr == nullptr)
        return;
    HazeLockGuard lock(mutex_);
    host_set_.erase(ptr);
}

void DeviceAllocator::reset() noexcept {
    HazeLockGuard lock(mutex_);
    map_.clear();
    shadow_data_.clear();
    pool_free_.clear();
    host_set_.clear();
    next_addr_ = kHbmBase;
    poly_bytes_ = 0;
}

} // namespace haze
