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

#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeGetDeviceCount(int *count) noexcept {
    if (count == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    *count = haze::device_count();
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeSetDevice(int device) noexcept {
    return set_error(haze::device_set_active(device));
}

extern "C" hazeError_t hazeGetDevice(int *device) noexcept {
    if (device == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    *device = haze::device_active();
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeGetDeviceProperties(hazeDeviceProp *prop, int device) noexcept {
    return set_error(haze::device_fill_properties(prop, device));
}

extern "C" hazeError_t hazeDeviceSynchronize() noexcept { return HAZE_SUCCESS; }

extern "C" hazeError_t hazeDeviceEnablePeerAccess(int /*peer*/, unsigned int /*flags*/) noexcept {
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeDeviceCanAccessPeer(int *can_access, int /*device*/,
                                               int /*peer*/) noexcept {
    if (can_access != nullptr)
        *can_access = 0;
    return set_error(HAZE_ERROR_NOT_SUPPORTED);
}

extern "C" hazeError_t hazeGetPerformanceCounters(void * /*counters*/) noexcept {
    return HAZE_SUCCESS;
}
