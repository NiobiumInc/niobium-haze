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

#include <haze/haze_types.h>

#include <cstdio>
#include <cstdlib>

namespace haze::detail {

hazeError_t to_public_error(HazeInternalError err) noexcept {
    switch (err) {
    case HazeInternalError::NotConfigured:
        return HAZE_ERROR_CONFIGERR;
    case HazeInternalError::UnknownAddress:
    case HazeInternalError::NoData:
    case HazeInternalError::AllocTooSmall:
        return HAZE_ERROR_INVALID_VALUE;
    case HazeInternalError::BackendError:
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_INVALID_VALUE;
}

namespace {

const char *internal_error_name(HazeInternalError err) noexcept {
    switch (err) {
    case HazeInternalError::NotConfigured:
        return "not configured";
    case HazeInternalError::UnknownAddress:
        return "unknown address";
    case HazeInternalError::NoData:
        return "no data";
    case HazeInternalError::AllocTooSmall:
        return "allocation too small";
    case HazeInternalError::BackendError:
        return "backend error";
    }
    return "unknown";
}

bool debug_logging_enabled() noexcept {
    static const bool enabled = []() {
        const char *v = std::getenv("HAZE_DEBUG");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

} // namespace

void record_internal_error(HazeInternalError err, const char *context) noexcept {
    if (!debug_logging_enabled())
        return;
    if (context != nullptr) {
        std::fprintf(stderr, "[haze] internal error: %s (%s)\n", internal_error_name(err), context);
    } else {
        std::fprintf(stderr, "[haze] internal error: %s\n", internal_error_name(err));
    }
}

} // namespace haze::detail
