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

#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "core/epoch.hpp"

#include <atomic>
#include <cstdint>

namespace haze {

// The single piece of mutable global state — the CUDA-primary-context analogue
// the C ABI implies. A Meyers singleton: constructed on first use, destroyed at
// exit in reverse member order; hazeDeviceReset via reset() is the mid-life teardown.
class DeviceState {
  public:
    static DeviceState &instance() noexcept;

    Config config;
    CompilerBackend backend;
    DeviceAllocator allocator;
    EpochState epoch;
    std::atomic<int> active_device{0};
    std::atomic<uint64_t> next_stream_id{1};
    std::atomic<uint64_t> next_event_id{1};

    // Full teardown backing hazeDeviceReset; subsystem order matters (see the .cpp).
    void reset() noexcept;

    // Rule of five, all deleted: the sole instance is reached by reference,
    // never copied or moved.
    DeviceState(const DeviceState &) = delete;
    DeviceState &operator=(const DeviceState &) = delete;
    DeviceState(DeviceState &&) = delete;
    DeviceState &operator=(DeviceState &&) = delete;
    ~DeviceState() = default;

  private:
    DeviceState() = default;
};

inline DeviceState &device_state() noexcept {
    return DeviceState::instance();
}

} // namespace haze
