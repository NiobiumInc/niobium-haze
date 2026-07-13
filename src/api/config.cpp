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
#include "core/config.hpp"

#include "common/errors.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeConfigureDevice(const hazeFheParams *fhe,
                                           const hazeReplayConfig *replay) noexcept {
    // One-shot configuration: the caller owns both structs. `fhe` is required;
    // `replay` may be null (all defaults). configure_device builds the immutable
    // configs through transient local builders and installs them — no config
    // state is held before this call, and nothing is installed on failure.
    if (fhe == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    return set_internal_result(haze::configure_device(*fhe, replay));
}
