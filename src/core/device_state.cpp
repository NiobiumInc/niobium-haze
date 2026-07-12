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
#include "core/device_state.hpp"

#include <atomic>
// Plain C-ABI header (only haze_types.h + stdint.h) — pulls no C++/OpenFHE, so
// depending on it here does not leak the bridge's OpenFHE surface into core/.
#include <haze/replay_bridge.h>

namespace haze {

DeviceState &DeviceState::instance() noexcept {
    // Meyers singleton: thread-safe first-use init, destroyed at exit. A throw
    // from the constructor terminates through this noexcept frame.
    static DeviceState inst;
    return inst;
}

void DeviceState::reset() noexcept {
    epoch.reset();
    backend.reset();
    allocator.reset();
    config.reset();
    active_device.store(0, std::memory_order_relaxed);
    next_stream_id.store(1, std::memory_order_relaxed);
    next_event_id.store(1, std::memory_order_relaxed);
    // Bridge reset queries the live compiler, so it must precede the vendor
    // compiler teardown that follows.
    hazeReplayBridgeReset();
    CompilerBackend::reset_compiler();
}

// Single definition point for the subsystem accessors declared in each
// class's header.
Config &config() noexcept {
    return device_state().config;
}
CompilerBackend &backend() noexcept {
    return device_state().backend;
}
DeviceAllocator &allocator() noexcept {
    return device_state().allocator;
}
EpochState &epoch() noexcept {
    return device_state().epoch;
}

} // namespace haze
