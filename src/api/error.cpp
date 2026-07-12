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
#include "common/errors.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

thread_local hazeError_t g_last_error = HAZE_SUCCESS;

extern "C" hazeError_t hazeGetLastError() noexcept {
    const hazeError_t err = g_last_error;
    g_last_error = HAZE_SUCCESS;
    return err;
}

extern "C" const char *hazeGetErrorString(hazeError_t error) noexcept {
    switch (error) {
    case HAZE_SUCCESS:
        return "no error";
    case HAZE_ERROR_INVALID_VALUE:
        return "invalid value";
    case HAZE_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case HAZE_ERROR_NOT_SUPPORTED:
        return "not supported";
    case HAZE_ERROR_CONFIGERR:
        return "configuration error";
    case HAZE_ERROR_UNKNOWN_ADDRESS:
        return "unknown device address";
    case HAZE_ERROR_NO_DATA:
        return "no data at device address";
    case HAZE_ERROR_SIZE_MISMATCH:
        return "size does not match configured polynomial size (ring_dim * sizeof(uint64_t))";
    case HAZE_ERROR_SOURCE_UNAVAILABLE:
        return "compute / D2D source was never written: hazeMemcpy(H2D) or compute into it first";
    case HAZE_ERROR_NOT_FLUSHED:
        return "D2H of an untagged / unflushed address (tag output + hazeFlush first)";
    case HAZE_ERROR_INTERNAL:
        return "internal haze error (set HAZE_DEBUG=1 for details)";
    default:
        return "unknown error";
    }
}
