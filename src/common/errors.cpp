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
    // User-actionable: the caller can fix their code.
    case HazeInternalError::InvalidArgument:
        return HAZE_ERROR_INVALID_VALUE;
    case HazeInternalError::NotConfigured:
    case HazeInternalError::ConfigLocked:
        return HAZE_ERROR_CONFIGERR;
    case HazeInternalError::UnknownAddress:
        return HAZE_ERROR_UNKNOWN_ADDRESS;
    case HazeInternalError::NoData:
        return HAZE_ERROR_NO_DATA;
    case HazeInternalError::PolySizeMismatch:
        return HAZE_ERROR_SIZE_MISMATCH;
    case HazeInternalError::SourceUnavailable:
        return HAZE_ERROR_SOURCE_UNAVAILABLE;
    case HazeInternalError::OutputNotFlushed:
        return HAZE_ERROR_NOT_FLUSHED;
    case HazeInternalError::UnsupportedDataFormat:
        return HAZE_ERROR_NOT_SUPPORTED;
    // Internal: haze invariants / backend failed. Caller can't recover;
    // the specific variant survives in the HAZE_DEBUG=1 stderr log.
    case HazeInternalError::BackendInitFailed:
    case HazeInternalError::BackendReplayFailed:
    case HazeInternalError::BackendShapeMismatch:
    case HazeInternalError::MrpGroupAddrModuliMismatch:
    case HazeInternalError::MissingPolyMapBinding:
    case HazeInternalError::ShadowSizeMismatch:
    case HazeInternalError::BackendOutputMissing:
    case HazeInternalError::BackendOutputDecodeFailed:
    case HazeInternalError::BridgeHookFailed:
    case HazeInternalError::PoolMapDesync:
        return HAZE_ERROR_INTERNAL;
    }
    // Unreachable: the switch above is exhaustive. If a new variant is
    // added without extending this table, "haze is broken" is the
    // correct user-visible classification.
    return HAZE_ERROR_INTERNAL;
}

namespace {

const char *internal_error_name(HazeInternalError err) noexcept {
    switch (err) {
    case HazeInternalError::InvalidArgument:
        return "invalid argument";
    case HazeInternalError::NotConfigured:
        return "not configured";
    case HazeInternalError::ConfigLocked:
        return "configuration locked (already configured / in use); conflicting re-set rejected";
    case HazeInternalError::UnknownAddress:
        return "unknown address";
    case HazeInternalError::NoData:
        return "no data (reserved; surfaced as source-unavailable)";
    case HazeInternalError::PolySizeMismatch:
        return "size does not match configured polynomial size (ring_dim * sizeof(uint64_t))";
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
    case HazeInternalError::BackendOutputMissing:
        return "backend output missing";
    case HazeInternalError::BackendOutputDecodeFailed:
        return "backend output decode failed";
    case HazeInternalError::BridgeHookFailed:
        return "post-recording hook reported failures";
    case HazeInternalError::PoolMapDesync:
        return "pool/map desync";
    case HazeInternalError::SourceUnavailable:
        return "compute / D2D source was never written: hazeMemcpy(H2D) or compute into it first";
    case HazeInternalError::OutputNotFlushed:
        return "D2H read of address with no materialized bytes (tag output + flush before D2H)";
    case HazeInternalError::UnsupportedDataFormat:
        return "montgomery/bit-reversal not supported on this target (use FUNC_SIM, not local)";
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
    if (debug_logging_enabled()) {
        if (context != nullptr) {
            std::println(stderr, "[haze] internal error: {} ({})", internal_error_name(err),
                         context);
        } else {
            std::println(stderr, "[haze] internal error: {}", internal_error_name(err));
        }
    }
}

} // namespace haze
