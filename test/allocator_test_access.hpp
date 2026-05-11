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
};

} // namespace haze::test
