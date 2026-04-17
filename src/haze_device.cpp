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
#include "haze_internal.hpp"

#include <cstring>

static constexpr int kDeviceCount = 1;
static constexpr size_t kHbmSize = 16ULL * 1024 * 1024 * 1024;  // 16 GB
static constexpr int kNumRegisters = 64;
static constexpr int kMaxCiphertextModuli = 64;
static constexpr int kNumHbmBanks = 8;
// Ring dimension exponents 10..16 → N = 1024..65536
static constexpr int kSupportedRingDimExponents[] = {10, 11, 12, 13, 14, 15, 16};
static constexpr int kNumSupportedRingDims =
    static_cast<int>(sizeof(kSupportedRingDimExponents) / sizeof(kSupportedRingDimExponents[0]));

static int g_active_device = 0;

extern "C" hazeError_t hazeGetDeviceCount(int* count) noexcept {
    if (!count) return set_error(HAZE_ERROR_INVALID_VALUE);
    *count = kDeviceCount;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSetDevice(int device) noexcept {
    if (device != 0) return set_error(HAZE_ERROR_INVALID_VALUE);
    g_active_device = device;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeGetDevice(int* device) noexcept {
    if (!device) return set_error(HAZE_ERROR_INVALID_VALUE);
    *device = g_active_device;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeGetDeviceProperties(hazeDeviceProp* prop,
                                                int device) noexcept {
    if (!prop) return set_error(HAZE_ERROR_INVALID_VALUE);
    if (device != 0) return set_error(HAZE_ERROR_INVALID_VALUE);

    *prop = {};
    std::strncpy(prop->name, "Niobium FPGA", sizeof(prop->name) - 1);
    prop->name[sizeof(prop->name) - 1] = '\0';
    prop->hbmSize = kHbmSize;
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

extern "C" hazeError_t hazeDeviceSynchronize() noexcept {
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeDeviceEnablePeerAccess(int /*peer*/,
                                                   unsigned int /*flags*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeDeviceCanAccessPeer(int* can_access,
                                                int /*device*/,
                                                int /*peer*/) noexcept {
    if (can_access) *can_access = 0;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGetPerformanceCounters(void* /*counters*/) noexcept {
    return HAZE_SUCCESS;
}
