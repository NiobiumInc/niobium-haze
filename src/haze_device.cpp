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
#include "haze_device.hpp"

#include <haze/haze_types.h>

#include <cstddef>
#include <cstring>

namespace haze {

namespace {
inline constexpr int kDeviceCount = 1;
inline constexpr size_t kHbmSize = 16ULL * 1024 * 1024 * 1024; // 16 GB
inline constexpr int kNumRegisters = 64;
inline constexpr int kMaxCiphertextModuli = 64;
inline constexpr int kNumHbmBanks = 8;
// Ring dimension exponents 10..16 → N = 1024..65536.
inline constexpr int kSupportedRingDimExponents[] = {10, 11, 12, 13, 14, 15, 16};
inline constexpr int kNumSupportedRingDims =
    static_cast<int>(sizeof(kSupportedRingDimExponents) / sizeof(kSupportedRingDimExponents[0]));

// Single-device runtime: only one piece of mutable state.
int g_active_device = 0;
} // namespace

int device_count() noexcept { return kDeviceCount; }

int device_active() noexcept { return g_active_device; }

hazeError_t device_set_active(int device) noexcept {
    if (device != 0)
        return HAZE_ERROR_INVALID_VALUE;
    g_active_device = device;
    return HAZE_SUCCESS;
}

hazeError_t device_fill_properties(hazeDeviceProp *prop, int device) noexcept {
    if (prop == nullptr)
        return HAZE_ERROR_INVALID_VALUE;
    if (device != 0)
        return HAZE_ERROR_INVALID_VALUE;

    *prop = {};
    std::strncpy(prop->name, "Niobium FPGA", sizeof(prop->name) - 1);
    prop->name[sizeof(prop->name) - 1] = '\0';
    prop->totalGlobalMem = kHbmSize;
    prop->numRegisters = kNumRegisters;
    prop->numSupportedRingDims = kNumSupportedRingDims;
    for (int i = 0; i < kNumSupportedRingDims; i++) {
        prop->supportedRingDimExponents[i] = kSupportedRingDimExponents[i];
    }
    prop->maxCiphertextModuli = kMaxCiphertextModuli;
    prop->numHBMBanks = kNumHbmBanks;
    prop->overlapCaps = HAZE_OVERLAP_FULL;
    prop->instructionFIFODepth = 256;
    return HAZE_SUCCESS;
}

void device_reset() noexcept { g_active_device = 0; }

} // namespace haze
