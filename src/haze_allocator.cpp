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

#include <haze/haze_types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <mutex>
#include <utility>
#include <vector>

namespace haze::detail {

DeviceAllocator &DeviceAllocator::instance() noexcept {
    static DeviceAllocator inst;
    return inst;
}

void DeviceAllocator::clear_pool_locked() noexcept {
    for (DevAddr addr : pool_free_) {
        map_.erase(addr);
    }
    pool_free_.clear();
}

void DeviceAllocator::set_polynomial_size(size_t bytes) noexcept {
    std::lock_guard lock(mutex_);
    if (poly_bytes_ == bytes)
        return;
    clear_pool_locked();
    poly_bytes_ = bytes;
}

size_t DeviceAllocator::polynomial_size() const noexcept {
    std::lock_guard lock(mutex_);
    return poly_bytes_;
}

std::expected<DevAddr, HazeInternalError> DeviceAllocator::allocate(size_t bytes) noexcept {
    std::lock_guard lock(mutex_);

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

    // Recycle from the free list when available.
    if (!pool_free_.empty()) {
        DevAddr addr = pool_free_.back();
        pool_free_.pop_back();
        auto it = map_.find(addr);
        if (it != map_.end()) {
            std::fill(it->second.host_shadow.begin(), it->second.host_shadow.end(), uint8_t{0});
            it->second.has_data = false;
            return addr;
        }
        // Pool free list out of sync with the map — fall through to a
        // fresh allocation. Should not happen in practice.
    }

    // Fresh allocation: bump the address counter by exactly poly_bytes_.
    DevAddr addr{next_addr_};
    next_addr_ += poly_bytes_;

    Allocation a{};
    a.addr = addr;
    a.size = poly_bytes_;
    a.host_shadow.resize(poly_bytes_, 0);
    a.has_data = false;
    a.pooled = true;
    map_.emplace(addr, std::move(a));
    return addr;
}

hazeError_t DeviceAllocator::free(DevAddr addr) noexcept {
    if (to_uintptr(addr) == 0)
        return HAZE_SUCCESS; // hazeFree(nullptr) matches CUDA semantics

    std::lock_guard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        record_internal_error(HazeInternalError::UnknownAddress, "DeviceAllocator::free");
        return HAZE_ERROR_INVALID_VALUE;
    }
    if (it->second.pooled) {
        pool_free_.push_back(addr);
    } else {
        map_.erase(it);
    }
    return HAZE_SUCCESS;
}

std::expected<std::vector<uint64_t>, HazeInternalError>
DeviceAllocator::extract_polynomial_components(DevAddr addr, uint64_t ring_dim) const noexcept {
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "DeviceAllocator::extract_polynomial_components: ring_dim == 0");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    std::lock_guard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        record_internal_error(HazeInternalError::UnknownAddress,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const Allocation &a = it->second;
    if (!a.has_data) {
        record_internal_error(HazeInternalError::NoData,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::NoData);
    }
    const size_t expected_bytes = ring_dim * sizeof(uint64_t);
    if (a.size < expected_bytes) {
        record_internal_error(HazeInternalError::AllocTooSmall,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::AllocTooSmall);
    }
    std::vector<uint64_t> components(ring_dim);
    std::memcpy(components.data(), a.host_shadow.data(), expected_bytes);
    return components;
}

hazeError_t DeviceAllocator::copy_h2d(DevAddr dst, const void *src, size_t count) noexcept {
    if (src == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    auto it = map_.find(dst);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    std::memcpy(it->second.host_shadow.data(), src, count);
    it->second.has_data = true;
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::copy_d2d(DevAddr dst, DevAddr src, size_t count) noexcept {
    std::lock_guard lock(mutex_);
    auto src_it = map_.find(src);
    auto dst_it = map_.find(dst);
    if (src_it == map_.end() || dst_it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > src_it->second.size || count > dst_it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    std::memcpy(dst_it->second.host_shadow.data(), src_it->second.host_shadow.data(), count);
    dst_it->second.has_data = src_it->second.has_data;
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::memset(DevAddr addr, int value, size_t count) noexcept {
    std::lock_guard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    std::memset(it->second.host_shadow.data(), value, count);
    it->second.has_data = true;
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::copy_to_host(void *dst, DevAddr src, size_t count) const noexcept {
    if (dst == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    std::lock_guard lock(mutex_);
    auto it = map_.find(src);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    if (count > it->second.size)
        return HAZE_ERROR_INVALID_VALUE;
    std::memcpy(dst, it->second.host_shadow.data(), count);
    return HAZE_SUCCESS;
}

hazeError_t DeviceAllocator::update_shadow(DevAddr addr,
                                           const std::vector<uint8_t> &bytes) noexcept {
    std::lock_guard lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end())
        return HAZE_ERROR_INVALID_VALUE;
    const size_t copy_len = std::min(bytes.size(), it->second.host_shadow.size());
    std::memcpy(it->second.host_shadow.data(), bytes.data(), copy_len);
    it->second.has_data = true;
    return HAZE_SUCCESS;
}

bool DeviceAllocator::is_device_pointer(const void *ptr) const noexcept {
    if (ptr == nullptr)
        return false;
    std::lock_guard lock(mutex_);
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
        std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    host_set_.insert(ptr);
}

void DeviceAllocator::unregister_host_pointer(const void *ptr) noexcept {
    if (ptr == nullptr)
        return;
    std::lock_guard lock(mutex_);
    host_set_.erase(ptr);
}

void DeviceAllocator::reset() noexcept {
    std::lock_guard lock(mutex_);
    map_.clear();
    pool_free_.clear();
    host_set_.clear();
    next_addr_ = kHbmBase;
    poly_bytes_ = 0;
}

} // namespace haze::detail
