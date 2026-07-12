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
//
// hazeDeviceReset and the flush/tag entry points; the reset orchestration
// itself lives in DeviceState::reset() (core/device_state.cpp).

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/device_state.hpp"
#include "core/epoch.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeDeviceReset(void) noexcept {
    haze::device_state().reset();
    // Match cudaDeviceReset: also clear the thread-local last-error so
    // callers can use this as a clean test-isolation point.
    g_last_error = HAZE_SUCCESS;
    return HAZE_SUCCESS;
}

extern "C" hazeError_t hazeWriteProgram(void) noexcept {
    return set_internal_result(haze::write_program());
}

extern "C" hazeError_t hazeTagOutput(void *ptr) noexcept {
    if (ptr == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::tag_output(haze::to_dev_addr(ptr)));
}

extern "C" hazeError_t hazeFlush(void) noexcept {
    return set_internal_result(haze::flush());
}
