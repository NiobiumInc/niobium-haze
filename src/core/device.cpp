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
#include "core/device.hpp"

#include "common/errors.hpp"
#include "core/device_state.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>

namespace haze {

namespace {
inline constexpr int kDeviceCount = 1;
inline constexpr size_t kHbmSize = 16ULL * 1024 * 1024 * 1024; // 16 GB
inline constexpr int kNumRegisters = 64;
inline constexpr int kNumHbmBanks = 8;
// Ring-dim exponents kMinRingDimExponent..kMaxRingDimExponent (N = 1024..65536);
// the envelope constants live in device.hpp so FheParams::create shares the range.
inline constexpr int kNumSupportedRingDims = kMaxRingDimExponent - kMinRingDimExponent + 1;

} // namespace

int device_count() noexcept {
    return kDeviceCount;
}

int device_active() noexcept {
    return device_state().active_device.load(std::memory_order_relaxed);
}

std::expected<void, HazeInternalError> device_set_active(int device) noexcept {
    if (device != 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    // active_device starts 0 and DeviceState::reset() only stores 0; the
    // guard above rejects every non-zero value, so no assignment is needed.
    return {};
}

std::expected<void, HazeInternalError> device_fill_properties(hazeDeviceProp *prop,
                                                              int device) noexcept {
    if (prop == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (device != 0)
        return std::unexpected(HazeInternalError::InvalidArgument);

    *prop = {};
    std::strncpy(prop->name, "Niobium FPGA", sizeof(prop->name) - 1);
    prop->name[sizeof(prop->name) - 1] = '\0';
    prop->totalGlobalMem = kHbmSize;
    prop->numRegisters = kNumRegisters;
    prop->numSupportedRingDims = kNumSupportedRingDims;
    for (int i = 0; i < kNumSupportedRingDims; i++) {
        prop->supportedRingDimExponents[i] = kMinRingDimExponent + i;
    }
    prop->maxCiphertextModuli = kMaxCiphertextModuli;
    prop->numHBMBanks = kNumHbmBanks;
    return {};
}

} // namespace haze
