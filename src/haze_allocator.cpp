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
#include "haze_allocator.hpp"

#include "haze_errors.hpp"
#include "haze_handle.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>
#include <mutex>
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
        if (map_.find(addr) != map_.end()) {
            return addr;
        }
        // Pool free list out of sync with the map — fall through to a
        // fresh allocation. Should not happen in practice.
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
    auto data_it = shadow_data_.find(addr);
    if (data_it == shadow_data_.end()) {
        // Address allocated but no bytes ever written — caller decides
        // whether to fall through to a zero polynomial via fhetch.
        record_internal_error(HazeInternalError::NoData,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::NoData);
    }
    std::vector<uint64_t> components(ring_dim);
    std::memcpy(components.data(), data_it->second.data(), expected_bytes);
    // Evict the shadow — fhetch::Polynomial::from_data (the caller's
    // next move) takes ownership of the bytes via shared_ptr<MRPImpl>.
    // The HAZE-side shadow becomes redundant; releasing it here frees
    // memory mid-program, before hazeFree.
    shadow_data_.erase(data_it);
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
    if (shadow.size() != it->second.size) {
        shadow.assign(it->second.size, uint8_t{0});
    }
    std::memcpy(shadow.data(), src, count);
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
    if (dst_shadow.size() != dst_it->second.size) {
        dst_shadow.assign(dst_it->second.size, uint8_t{0});
    }
    std::memcpy(dst_shadow.data(), src_data->second.data(), count);
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
    if (shadow.size() != it->second.size) {
        shadow.assign(it->second.size, uint8_t{0});
    }
    std::memset(shadow.data(), value, count);
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
    std::memcpy(dst, data_it->second.data(), count);
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::update_shadow(DevAddr addr,
                                           const std::vector<uint8_t> &bytes) noexcept {
    HazeLockGuard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    auto &shadow = shadow_data_[addr];
    if (shadow.size() != it->second.size) {
        shadow.assign(it->second.size, uint8_t{0});
    }
    const size_t copy_len = std::min(bytes.size(), shadow.size());
    std::memcpy(shadow.data(), bytes.data(), copy_len);
    return HAZE_SUCCESS;
}

bool DeviceAllocator::is_device_pointer(const void *ptr) const noexcept {
    if (ptr == nullptr)
        return false;
    HazeLockGuard lock(mutex_);
    return map_.find(to_dev_addr(ptr)) != map_.end();
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
    enum { kUnknown, kDevice, kHost } classification = kUnknown;
    if (ptr != nullptr) {
        HazeLockGuard lock(mutex_);
        if (map_.find(to_dev_addr(ptr)) != map_.end()) {
            classification = kDevice;
        } else if (host_set_.find(ptr) != host_set_.end()) {
            classification = kHost;
        }
    }
    switch (classification) {
    case kDevice:
        attrs->type = HAZE_MEMORY_TYPE_DEVICE;
        attrs->devicePointer = const_cast<void *>(ptr);
        break;
    case kHost:
        attrs->type = HAZE_MEMORY_TYPE_HOST;
        attrs->hostPointer = const_cast<void *>(ptr);
        break;
    case kUnknown:
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
