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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <string>
#include <string_view>
#include <utility>

// Thread-local last error with CUDA sticky semantics: failures overwrite,
// successes leave it untouched, hazeGetLastError reads-and-clears.
extern thread_local hazeError_t g_last_error;

// The detail behind g_last_error, paired 1:1 with it by set_error; "" when
// that failure recorded none. hazeGetLastErrorMessage reads and clears it,
// and hazeDeviceReset clears it.
extern thread_local std::string g_last_error_message;

// Detail recorded for the failure in progress, not yet committed by
// set_error. record_internal_error and haze::log_error append to it,
// independent of HAZE_DEBUG; the _warning sinks never do. Anything left here
// surfaces in the next failure, so a caller that ignores a failure discards
// its detail (DiscardErrorDetail). hazeDeviceReset clears it.
extern thread_local std::string g_error_detail;

// Appends `detail` to g_error_detail, joined by "; " (innermost cause first).
// No-op for an empty detail.
inline void append_error_detail(std::string_view detail) noexcept {
    if (detail.empty())
        return;
    if (!g_error_detail.empty())
        g_error_detail += "; ";
    g_error_detail += detail;
}

// Discards whatever g_error_detail gains during its lifetime. Wrap a call whose
// failure the caller ignores: shared helpers (InputSpillStore::erase,
// CompilerBackend::clear_captured, ...) record their failures for fatal
// callers, so only the ignoring call site knows to drop the detail.
class DiscardErrorDetail {
  public:
    DiscardErrorDetail() noexcept : mark_(g_error_detail.size()) {}
    ~DiscardErrorDetail() noexcept {
        // An inner set_error may have drained the slot below mark_, and
        // resize() would then pad it with NULs.
        if (g_error_detail.size() > mark_)
            g_error_detail.resize(mark_);
    }

    DiscardErrorDetail(const DiscardErrorDetail &) = delete;
    DiscardErrorDetail &operator=(const DiscardErrorDetail &) = delete;

  private:
    std::size_t mark_;
};

// Commits a new hazeError_t; every failing hazeXxx() call returns through here,
// directly or via set_internal_result. Moves the pending detail into
// g_last_error_message, so each code carries only its own failure's detail.
[[nodiscard]] inline hazeError_t set_error(hazeError_t err) noexcept {
    g_last_error = err;
    g_last_error_message = std::exchange(g_error_detail, std::string{});
    return err;
}

namespace haze {

// Internal error type: `to_public_error` maps each variant to a granular
// `hazeError_t`, and internal callers keep the typed enum, translating only at the
// C ABI boundary.
enum class HazeInternalError : std::uint8_t {
    InvalidArgument,            // params struct field violates the API contract
    NotConfigured,              // ring_dim / modulus not set when required
    ConfigLocked,               // configuration frozen; conflicting re-set rejected
    DuplicateModulus,           // FheParams::build(): two ciphertext moduli are equal
    CompositeModulus,           // FheParams::build(): a ciphertext modulus is not prime
    UnknownAddress,             // DevAddr not in the allocator's table
    NoData,                     // address allocated but no H2D / compute output present
    PolySizeMismatch,           // size != configured polynomial size (ring_dim * 8)
    BackendInitFailed,          // niobium::compiler() initialization threw
    BackendReplayFailed,        // niobium::compiler() stop / replay returned false or threw
    BackendShapeMismatch,       // backend returned a result with unexpected shape / length
    MrpGroupAddrModuliMismatch, // MRP group registration: addrs / moduli span lengths differ
    MissingPolyMapBinding,      // pending output / MRP group addr is not in poly_map_
    ShadowSizeMismatch,         // shadow buffer length disagrees with ring_dim invariant
    BackendOutputMissing,       // fhetch::result(name, ...) returned false
    BackendOutputDecodeFailed,  // decode_result_values returned false
    BridgeHookFailed,           // replay_bridge post-recording hook reported failures
    PoolMapDesync,              // pool_free_ entry has no `alloc_set_` peer
    SourceUnavailable,          // compute / D2D source has no shadow data and no poly_map_ binding
    OutputNotFlushed,           // D2H of an address with no materialized bytes: tag + flush first
    SpillIoFailed,              // input spill store I/O or misuse (write/read/create_directories)
    SpillRecordMissing          // spill record not found for a tagged input name
};

// Map an internal error to the public hazeError_t; adding a variant requires
// extending this table.
hazeError_t to_public_error(HazeInternalError err) noexcept;

// Record a failure reason: appends it to g_error_detail and prints it to stderr
// under HAZE_DEBUG=1. The context string is unowned and short-lived. Use
// record_internal_warning when the caller does not go on to fail.
void record_internal_error(HazeInternalError err, const char *context = nullptr) noexcept;

// Same as record_internal_error but never appends to g_error_detail, for a
// diagnosis whose caller does not fail (e.g. best-effort cleanup).
void record_internal_warning(HazeInternalError err, const char *context = nullptr) noexcept;

} // namespace haze

// Translate an internal-error result at the C ABI boundary; success does
// not touch the sticky g_last_error.
[[nodiscard]] inline hazeError_t
set_internal_result(std::expected<void, haze::HazeInternalError> result) noexcept {
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    return HAZE_SUCCESS;
}
