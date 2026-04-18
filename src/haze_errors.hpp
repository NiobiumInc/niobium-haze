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

#include <haze/haze_types.h>

// Thread-local last-error state. set_error is the inline writer used at
// every C ABI boundary; the public hazeGetLastError reads and clears.
extern thread_local hazeError_t g_last_error;

[[nodiscard]] inline hazeError_t set_error(hazeError_t err) noexcept {
    g_last_error = err;
    return err;
}

namespace haze::detail {

// Internal error type. Multiple internal causes can map to the same
// public hazeError_t code (e.g. UnknownAddress / NoData / AllocTooSmall
// all surface as HAZE_ERROR_INVALID_VALUE), so the public type would
// erase information internal callers and debug logs need. Keep the
// typed enum for that discrimination; map at the C ABI boundary only.
enum class HazeInternalError {
    NotConfigured,  // ring_dim / modulus not set when required
    UnknownAddress, // DevAddr not in the allocator's table
    NoData,         // address allocated but no H2D / compute output present
    AllocTooSmall,  // allocation size < polynomial size
    BackendError,   // niobium::compiler() / fhetch returned failure
};

// Map an internal error to the public hazeError_t the C ABI returns.
// Adding a new internal error variant requires extending this table.
hazeError_t to_public_error(HazeInternalError) noexcept;

// Record a failure reason for HAZE_DEBUG=1 logging. Prints to stderr
// when the env var is set. The context string is unowned and short-lived.
void record_internal_error(HazeInternalError err, const char *context = nullptr) noexcept;

} // namespace haze::detail
