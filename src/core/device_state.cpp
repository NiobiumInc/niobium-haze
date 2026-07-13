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

#include "common/errors.hpp"
#include "core/config.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <utility>
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
    fhe_params = {}; // back to unconfigured; hazeConfigureDevice must run again
    replay_config = {};
    configured = false;
    active_device.store(0, std::memory_order_relaxed);
    next_stream_id.store(1, std::memory_order_relaxed);
    next_event_id.store(1, std::memory_order_relaxed);
    // Bridge reset queries the live compiler, so it must precede the vendor
    // compiler teardown that follows.
    hazeReplayBridgeReset();
    CompilerBackend::reset_compiler();
}

// Single definition point for the subsystem accessors declared in each header.
CompilerBackend &backend() noexcept {
    return device_state().backend;
}
DeviceAllocator &allocator() noexcept {
    return device_state().allocator;
}
EpochState &epoch() noexcept {
    return device_state().epoch;
}

// Read side: the frozen configs — plain always-valid members (default until
// configured), so these never fault; callers gate on config_finalized().
const FheParams &fhe_params() noexcept {
    return device_state().fhe_params;
}
const ReplayConfig &replay_config() noexcept {
    return device_state().replay_config;
}

bool config_finalized() noexcept {
    return device_state().configured;
}

std::expected<void, HazeInternalError> configure_device(const hazeFheParams &fhe,
                                                        const hazeReplayConfig *replay) noexcept {
    // Once the backend is brought up (first compute) it has latched the config,
    // so a later reconfigure could not take effect — reject it rather than
    // silently diverge. Reconfiguring before bring-up is fine (latest wins);
    // hazeDeviceReset re-opens configuration.
    if (backend().is_initialized()) {
        record_internal_error(
            HazeInternalError::ConfigLocked,
            "hazeConfigureDevice: device already brought up; reset to reconfigure");
        return std::unexpected(HazeInternalError::ConfigLocked);
    }

    // Validate + deep-copy the caller's structs into the immutable configs; the
    // config types own all validation (no builder). Nothing is installed unless
    // FheParams::create succeeds (ReplayConfig::create is infallible).
    auto built_fhe = FheParams::create(fhe);
    if (!built_fhe)
        return std::unexpected(built_fhe.error());
    ReplayConfig built_replay = ReplayConfig::create(replay);

    // Apply the allocator's polynomial size only now the whole config is valid;
    // changing the ring dimension under live allocations would leave their
    // shadows mis-sized (a former D2H overread), so refuse that.
    const uint64_t ring_dim = built_fhe->ring_dim();
    if (allocator().has_live_allocations() && ring_dim != device_state().fhe_params.ring_dim()) {
        record_internal_error(HazeInternalError::ConfigLocked,
                              "hazeConfigureDevice: allocations live; free them or reset first");
        return std::unexpected(HazeInternalError::ConfigLocked);
    }
    allocator().set_polynomial_size(ring_dim * sizeof(uint64_t));

    // Commit the frozen configs — the only stored config state.
    device_state().fhe_params = std::move(*built_fhe);
    device_state().replay_config = std::move(built_replay);
    device_state().configured = true;
    return {};
}

} // namespace haze
