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

#include <cstdio> // IWYU pragma: keep — stderr macro lives here per ISO C++; libstdc++ leaks it through <print> which include-cleaner picks up first
#include <cstdlib>
#include <haze/haze_types.h>
#include <print>

namespace haze {

hazeError_t to_public_error(HazeInternalError err) noexcept {
    switch (err) {
    case HazeInternalError::NotConfigured:
        return HAZE_ERROR_CONFIGERR;
    case HazeInternalError::InvalidArgument:
    case HazeInternalError::UnknownAddress:
    case HazeInternalError::NoData:
    case HazeInternalError::AllocTooSmall:
        return HAZE_ERROR_INVALID_VALUE;
    case HazeInternalError::BackendInitFailed:
    case HazeInternalError::BackendReplayFailed:
    case HazeInternalError::BackendShapeMismatch:
    case HazeInternalError::MrpGroupAddrModuliMismatch:
    case HazeInternalError::MissingPolyMapBinding:
    case HazeInternalError::ShadowSizeMismatch:
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_INVALID_VALUE;
}

namespace {

const char *internal_error_name(HazeInternalError err) noexcept {
    switch (err) {
    case HazeInternalError::InvalidArgument:
        return "invalid argument";
    case HazeInternalError::NotConfigured:
        return "not configured";
    case HazeInternalError::UnknownAddress:
        return "unknown address";
    case HazeInternalError::NoData:
        return "no data";
    case HazeInternalError::AllocTooSmall:
        return "allocation too small";
    case HazeInternalError::BackendInitFailed:
        return "backend init failed";
    case HazeInternalError::BackendReplayFailed:
        return "backend replay failed";
    case HazeInternalError::BackendShapeMismatch:
        return "backend returned unexpected shape";
    case HazeInternalError::MrpGroupAddrModuliMismatch:
        return "MRP group addrs/moduli span size mismatch";
    case HazeInternalError::MissingPolyMapBinding:
        return "addr missing from poly_map_";
    case HazeInternalError::ShadowSizeMismatch:
        return "shadow buffer length != ring_dim";
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
        std::println(stderr, "[haze] internal error: {} ({})", internal_error_name(err), context);
    } else {
        std::println(stderr, "[haze] internal error: {}", internal_error_name(err));
    }
}

} // namespace haze
