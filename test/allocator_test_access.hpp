// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Test peer for DeviceAllocator (Attorney pattern); lives under test/
// and never compiles into the production library. Forward decl + friend
// grant live in src/core/allocator.hpp.
#pragma once

#include "core/allocator.hpp"

#include <cstdint>
#include <utility>

namespace haze::test {

class AllocatorTestAccess {
  public:
    // Runs `f(data, size)` over the shadow buffer for `addr` while
    // holding the allocator mutex; returns true iff a shadow entry
    // exists. The pointer is only valid inside the callback.
    template <typename F>
    static bool with_shadow_data(const DeviceAllocator &a, DevAddr addr, F &&f) noexcept {
        HazeLockGuard lock(a.mutex_);
        auto it = a.shadow_data_.find(addr);
        if (it == a.shadow_data_.end()) {
            return false;
        }
        std::forward<F>(f)(it->second.data(), it->second.size());
        return true;
    }

    // Number of live DevAddrs (alloc_set_ membership). Lets a test assert
    // that a failed allocation leaves nothing allocated.
    static std::size_t alloc_set_size(const DeviceAllocator &a) noexcept {
        HazeLockGuard lock(a.mutex_);
        return a.alloc_set_.size();
    }

    // Push a DevAddr to the FRONT of the free list. allocate pops from the
    // back, so a free list of [wedge, recyclable] makes a two-poly batch
    // recycle `recyclable` first, then pop `wedge`. Passing a still-LIVE addr
    // as `wedge` makes that second pop hit the "recycled addr already live"
    // PoolMapDesync path — the only way to exercise allocate_many's mid-batch
    // rollback (unreachable via the public API otherwise). Depends on the
    // allocator's LIFO free-list discipline.
    static void push_front_pool_entry(DeviceAllocator &a, DevAddr addr) noexcept {
        HazeLockGuard lock(a.mutex_);
        a.pool_free_.insert(a.pool_free_.begin(), addr);
    }
};

} // namespace haze::test
