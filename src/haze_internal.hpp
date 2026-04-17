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

#include <haze/haze.h>

// Thread-local last error, defined in haze_error.cpp.
extern thread_local hazeError_t g_last_error;

// Set the thread-local error and return it so callers can write:
//   return set_error(HAZE_ERROR_INVALID_VALUE);
[[nodiscard]] inline hazeError_t set_error(hazeError_t err) noexcept {
    g_last_error = err;
    return err;
}
