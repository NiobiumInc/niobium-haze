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

#include <cstdint>
#include <haze/haze_types.h>

// Thread-local last-error state. set_error is the inline writer used at
// every C ABI boundary; the public hazeGetLastError reads and clears.
extern thread_local hazeError_t g_last_error;

[[nodiscard]] inline hazeError_t set_error(hazeError_t err) noexcept {
    g_last_error = err;
    return err;
}

namespace haze {

// Internal error type. `to_public_error` maps each variant to a granular
// `hazeError_t`; internal callers keep the typed enum and translate only
// at the C ABI boundary.
enum class HazeInternalError : std::uint8_t {
    InvalidArgument,            // params struct field violates the API contract
    NotConfigured,              // ring_dim / modulus not set when required
    UnknownAddress,             // DevAddr not in the allocator's table
    NoData,                     // address allocated but no H2D / compute output present
    AllocTooSmall,              // allocation size < polynomial size
    BackendInitFailed,          // niobium::compiler() initialization threw
    BackendReplayFailed,        // niobium::compiler() stop_epoch / replay returned false or threw
    BackendShapeMismatch,       // backend returned a result with unexpected shape / length
    MrpGroupAddrModuliMismatch, // MRP group registration: addrs / moduli span lengths differ
    MissingPolyMapBinding,      // pending output / MRP group addr is not in poly_map_
    ShadowSizeMismatch,         // shadow buffer length disagrees with ring_dim invariant
    BackendOutputMissing,       // fhetch::result(name, ...) returned false
    BackendOutputDecodeFailed,  // extract_polynomial_values returned false
    BridgeHookFailed,           // replay_bridge post-recording hook reported failures
    PoolMapDesync,              // pool_free_ entry has no `alloc_set_` peer
    SourceUnavailable           // compute / D2D source has no shadow data and no poly_map_ binding
};

// Map an internal error to the public hazeError_t the C ABI returns.
// Adding a new internal error variant requires extending this table.
hazeError_t to_public_error(HazeInternalError err) noexcept;

// Record a failure reason for HAZE_DEBUG=1 logging (prints to stderr when set).
// The context string is unowned and short-lived.
void record_internal_error(HazeInternalError err, const char *context = nullptr) noexcept;

} // namespace haze
