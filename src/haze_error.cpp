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
#include "haze_errors.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

thread_local hazeError_t g_last_error = HAZE_SUCCESS;

extern "C" hazeError_t hazeGetLastError() noexcept {
    const hazeError_t err = g_last_error;
    g_last_error = HAZE_SUCCESS;
    return err;
}

extern "C" const char* hazeGetErrorString(hazeError_t error) noexcept {
    switch (error) {
        case HAZE_SUCCESS:              return "no error";
        case HAZE_ERROR_INVALID_HANDLE: return "invalid handle";
        case HAZE_ERROR_INVALID_VALUE:  return "invalid value";
        case HAZE_ERROR_OUT_OF_MEMORY:  return "out of memory";
        case HAZE_ERROR_NOT_SUPPORTED:  return "not supported";
        case HAZE_ERROR_NOT_READY:      return "device not ready";
        case HAZE_ERROR_LAUNCH_FAILURE: return "compilation or execution failure";
        case HAZE_ERROR_DMEMERR:        return "data memory error";
        case HAZE_ERROR_IMEMERR:        return "instruction memory error";
        case HAZE_ERROR_INSTRERR:       return "instruction error";
        case HAZE_ERROR_CONFIGERR:      return "configuration error";
        case HAZE_ERROR_ISEQERR:        return "instruction sequence error";
        default:                        return "unknown error";
    }
}
