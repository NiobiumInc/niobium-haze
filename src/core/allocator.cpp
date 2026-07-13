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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>
#include <span>
#include <utility>
#include <vector>

namespace haze {

void DeviceAllocator::clear_pool_locked() noexcept {
    for (DevAddr addr : pool_free_) {
        alloc_set_.erase(addr);
        shadow_data_.erase(addr);
    }
    pool_free_.clear();
}

void DeviceAllocator::set_polynomial_size(size_t bytes) noexcept {
    HazeLockGuard lock(mutex_);
    if (poly_bytes_ != bytes) {
        clear_pool_locked();
        poly_bytes_ = bytes;
    }
}

size_t DeviceAllocator::polynomial_size() const noexcept {
    HazeLockGuard lock(mutex_);
    return poly_bytes_;
}

std::expected<void, HazeInternalError>
DeviceAllocator::validate_poly_size_locked(size_t bytes) const noexcept {
    // Polynomial size must be configured (via hazeConfigureDevice)
    // before any allocation. HAZE's device space is FHETCH-addressable
    // storage — without a known polynomial size we cannot serve it.
    if (poly_bytes_ == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "DeviceAllocator::allocate: ring_dim unset");
        return std::unexpected(HazeInternalError::NotConfigured);
    }

    // Strict single-size contract: every allocation is one polynomial (non-poly
    // scratch uses host memory), so a size mismatch is rejected here.
    if (bytes != poly_bytes_) {
        record_internal_error(HazeInternalError::PolySizeMismatch,
                              "DeviceAllocator::allocate: size != polynomial bytes");
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    }
    return {};
}

std::expected<void, HazeInternalError>
DeviceAllocator::require_allocated(DevAddr addr) const noexcept {
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(addr)) {
        record_internal_error(HazeInternalError::UnknownAddress,
                              "DeviceAllocator::require_allocated");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    return {};
}

bool DeviceAllocator::has_live_allocations() const noexcept {
    HazeLockGuard lock(mutex_);
    return !alloc_set_.empty();
}

void DeviceAllocator::evict_shadow(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    shadow_data_.erase(addr);
}

std::expected<DevAddr, HazeInternalError> DeviceAllocator::allocate_one_locked() noexcept {
    // Recycle from the free list; a pooled DevAddr must not be live (its shadow and
    // alloc_set_ membership were dropped at hazeFree), so a live one means
    // pool_free_/alloc_set_ desynced.
    if (!pool_free_.empty()) {
        DevAddr addr = pool_free_.back();
        pool_free_.pop_back();
        if (alloc_set_.contains(addr)) {
            record_internal_error(HazeInternalError::PoolMapDesync,
                                  "DeviceAllocator::allocate (recycled addr already live)");
            return std::unexpected(HazeInternalError::PoolMapDesync);
        }
        alloc_set_.insert(addr);
        return addr;
    }

    // Fresh allocation: bump next_addr_ by poly_bytes_; no shadow bytes until the
    // first H2D/memset/D2D/update_shadow.
    DevAddr addr{next_addr_};
    next_addr_ += poly_bytes_;
    alloc_set_.insert(addr);
    return addr;
}

std::expected<void, HazeInternalError> DeviceAllocator::free_one_locked(DevAddr addr) noexcept {
    if (!alloc_set_.contains(addr)) {
        // Not live (unknown addr or double free): reject rather than re-pool, which
        // would later alias the same DevAddr across two live allocations.
        record_internal_error(HazeInternalError::UnknownAddress, "DeviceAllocator::free");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    // Lifetime ends: drop liveness and shadow bytes, pool the DevAddr for recycling.
    alloc_set_.erase(addr);
    shadow_data_.erase(addr);
    pool_free_.push_back(addr);
    return {};
}

std::expected<DevAddr, HazeInternalError> DeviceAllocator::allocate(size_t bytes) noexcept {
    HazeLockGuard lock(mutex_);
    if (auto ok = validate_poly_size_locked(bytes); !ok) {
        return std::unexpected(ok.error());
    }
    return allocate_one_locked();
}

std::expected<std::vector<DevAddr>, HazeInternalError>
DeviceAllocator::allocate_many(size_t count, size_t bytes) noexcept {
    std::vector<DevAddr> out;
    if (count == 0) {
        return out; // Empty MRP group: nothing to reserve.
    }
    HazeLockGuard lock(mutex_);
    if (auto ok = validate_poly_size_locked(bytes); !ok) {
        return std::unexpected(ok.error());
    }
    // The C-ABI shim bounds `count` to kMaxCiphertextModuli before calling in,
    // so this reservation can never request an unbounded size.
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto addr = allocate_one_locked();
        if (!addr) {
            // Roll back everything reserved in this call so nothing leaks; each was
            // just reserved, so free_one_locked cannot fail.
            for (DevAddr done : out) {
                [[maybe_unused]] const auto rolled_back = free_one_locked(done);
            }
            return std::unexpected(addr.error());
        }
        out.push_back(*addr);
    }
    return out;
}

std::expected<void, HazeInternalError> DeviceAllocator::free(DevAddr addr) noexcept {
    if (to_uintptr(addr) == 0) {
        // hazeFree(nullptr) matches CUDA semantics: silent success.
        return {};
    }
    HazeLockGuard lock(mutex_);
    return free_one_locked(addr);
}

std::expected<void, HazeInternalError>
DeviceAllocator::free_many(std::span<const DevAddr> addrs) noexcept {
    if (addrs.empty()) {
        return {};
    }
    HazeLockGuard lock(mutex_);
    std::expected<void, HazeInternalError> first_error{};
    for (DevAddr addr : addrs) {
        if (to_uintptr(addr) == 0) {
            continue; // Null entry: skip, matching hazeFree(nullptr).
        }
        // Keep the FIRST error: a truthy expected means no error recorded yet.
        if (auto freed = free_one_locked(addr); !freed && first_error.has_value()) {
            first_error = std::unexpected(freed.error());
        }
    }
    return first_error;
}

std::expected<std::vector<uint64_t>, HazeInternalError>
DeviceAllocator::extract_polynomial_components(DevAddr addr, uint64_t ring_dim) noexcept {
    // ring_dim is validated non-zero at FheParams::build(); callers only reach
    // here post-finalize, so no ring_dim==0 guard is needed.
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(addr)) {
        record_internal_error(HazeInternalError::UnknownAddress,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const size_t expected_bytes = ring_dim * sizeof(uint64_t);
    if (poly_bytes_ < expected_bytes) {
        record_internal_error(HazeInternalError::PolySizeMismatch,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    }
    auto data_it = shadow_data_.find(addr);
    if (data_it == shadow_data_.end()) {
        // Allocated but never written; lookup_or_create_locked surfaces this as
        // SourceUnavailable (compute/D2D on an uninitialized buffer is a violation).
        record_internal_error(HazeInternalError::NoData,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::NoData);
    }
    if (data_it->second.size() != ring_dim) {
        // Invariant break; checked before extraction so the error path
        // never destroys the caller's bytes.
        record_internal_error(HazeInternalError::ShadowSizeMismatch,
                              "DeviceAllocator::extract_polynomial_components");
        return std::unexpected(HazeInternalError::ShadowSizeMismatch);
    }
    // Detach the storage so the caller can move it into a Polynomial, freeing the
    // HAZE-side entry mid-program.
    auto node = shadow_data_.extract(data_it);
    return std::move(node.mapped());
}

std::expected<std::vector<uint64_t>, HazeInternalError>
DeviceAllocator::read_polynomial_components(DevAddr addr, uint64_t ring_dim) const noexcept {
    // ring_dim is validated non-zero at FheParams::build() (see extract).
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(addr)) {
        record_internal_error(HazeInternalError::UnknownAddress,
                              "DeviceAllocator::read_polynomial_components");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const size_t expected_bytes = ring_dim * sizeof(uint64_t);
    if (poly_bytes_ < expected_bytes) {
        record_internal_error(HazeInternalError::PolySizeMismatch,
                              "DeviceAllocator::read_polynomial_components");
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    }
    auto data_it = shadow_data_.find(addr);
    if (data_it == shadow_data_.end()) {
        record_internal_error(HazeInternalError::NoData,
                              "DeviceAllocator::read_polynomial_components");
        return std::unexpected(HazeInternalError::NoData);
    }
    if (data_it->second.size() != ring_dim) {
        record_internal_error(HazeInternalError::ShadowSizeMismatch,
                              "DeviceAllocator::read_polynomial_components");
        return std::unexpected(HazeInternalError::ShadowSizeMismatch);
    }
    return data_it->second;
}

std::expected<void, HazeInternalError> DeviceAllocator::copy_h2d(DevAddr dst, const void *src,
                                                                 size_t count) noexcept {
    if (src == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(dst))
        return std::unexpected(HazeInternalError::UnknownAddress);
    if (count > poly_bytes_)
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    // Zero-byte copy: success no-op, no fabricated shadow entry.
    if (count == 0)
        return {};
    // Lazy-create or overwrite the shadow entry, sized to the full allocation so a
    // partial write leaves the tail zeroed.
    auto &shadow = shadow_data_[dst];
    const size_t want_elems = poly_bytes_ / sizeof(uint64_t);
    if (shadow.size() != want_elems) {
        shadow.assign(want_elems, uint64_t{0});
    }
    std::memcpy(reinterpret_cast<uint8_t *>(shadow.data()), src, count);
    return {};
}

std::expected<void, HazeInternalError> DeviceAllocator::memset(DevAddr addr, int value,
                                                               size_t count) noexcept {
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(addr))
        return std::unexpected(HazeInternalError::UnknownAddress);
    if (count > poly_bytes_)
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    // Zero-byte memset: success no-op, no fabricated shadow entry.
    if (count == 0)
        return {};
    auto &shadow = shadow_data_[addr];
    const size_t want_elems = poly_bytes_ / sizeof(uint64_t);
    if (shadow.size() != want_elems) {
        shadow.assign(want_elems, uint64_t{0});
    }
    std::memset(reinterpret_cast<uint8_t *>(shadow.data()), value, count);
    return {};
}

std::expected<void, HazeInternalError> DeviceAllocator::copy_to_host(void *dst, DevAddr src,
                                                                     size_t count) const noexcept {
    if (dst == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(src))
        return std::unexpected(HazeInternalError::UnknownAddress);
    if (count > poly_bytes_)
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    if (count == 0)
        return {}; // zero-byte read: success no-op, no shadow required
    auto data_it = shadow_data_.find(src);
    if (data_it == shadow_data_.end()) {
        // No materialized bytes (never written, or a computed result not tagged +
        // flushed): an error rather than a silent zero read.
        record_internal_error(
            HazeInternalError::OutputNotFlushed,
            "DeviceAllocator::copy_to_host: no shadow bytes (tag output + flush?)");
        return std::unexpected(HazeInternalError::OutputNotFlushed);
    }
    // Bound by the entry's actual size so a stale-sized shadow can never
    // become a heap overread (should be unreachable given the config freeze).
    if (count > data_it->second.size() * sizeof(uint64_t)) {
        record_internal_error(HazeInternalError::ShadowSizeMismatch,
                              "DeviceAllocator::copy_to_host: shadow smaller than count");
        return std::unexpected(HazeInternalError::ShadowSizeMismatch);
    }
    std::memcpy(dst, reinterpret_cast<const uint8_t *>(data_it->second.data()), count);
    return {};
}

std::expected<void, HazeInternalError>
DeviceAllocator::update_shadow(DevAddr addr, std::vector<uint64_t> &&values) noexcept {
    HazeLockGuard lock(mutex_);
    if (!alloc_set_.contains(addr)) {
        record_internal_error(HazeInternalError::UnknownAddress, "DeviceAllocator::update_shadow");
        return std::unexpected(HazeInternalError::UnknownAddress);
    }
    const size_t want_elems = poly_bytes_ / sizeof(uint64_t);
    if (values.size() != want_elems) {
        values.resize(want_elems, uint64_t{0});
    }
    shadow_data_.insert_or_assign(addr, std::move(values));
    return {};
}

std::expected<void, HazeInternalError>
DeviceAllocator::pointer_attributes(hazePointerAttributes *attrs, const void *ptr) const noexcept {
    if (attrs == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    *attrs = {};
    // Resolve classification under one locked snapshot, then fill fields outside the
    // lock, closing the window where a concurrent free()/unregister could flip it.
    enum class Classification : uint8_t { Unknown, Device, Host };
    auto classification = Classification::Unknown;
    if (ptr != nullptr) {
        HazeLockGuard lock(mutex_);
        if (alloc_set_.contains(to_dev_addr(ptr))) {
            classification = Classification::Device;
        } else if (host_set_.contains(ptr)) {
            classification = Classification::Host;
        }
    }
    switch (classification) {
    case Classification::Device:
        attrs->type = HAZE_MEMORY_TYPE_DEVICE;
        // hazePointerAttributes mirrors cudaPointerAttributes: its devicePointer/
        // hostPointer are non-const void*, so const_cast the const input at this
        // C-ABI boundary.
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
    return {};
}

void DeviceAllocator::register_host_pointer(const void *ptr) noexcept {
    if (ptr != nullptr) {
        HazeLockGuard lock(mutex_);
        host_set_.insert(ptr);
    }
}

bool DeviceAllocator::unregister_host_pointer(const void *ptr) noexcept {
    if (ptr == nullptr)
        return false;
    HazeLockGuard lock(mutex_);
    return host_set_.erase(ptr) > 0;
}

void DeviceAllocator::reset() noexcept {
    HazeLockGuard lock(mutex_);
    alloc_set_.clear();
    shadow_data_.clear();
    pool_free_.clear();
    host_set_.clear();
    next_addr_ = kHbmBase;
    poly_bytes_ = 0;
}

} // namespace haze
