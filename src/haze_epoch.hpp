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

#include "haze_errors.hpp"
#include "haze_handle.hpp"

#include <haze/haze_types.h>

#include <niobium/fhetch_api.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <unordered_map>

namespace haze::detail {

// Epoch state is the single point of contact between the compute API
// and the niobium compiler's recording window. It tracks the active
// polymap (DevAddr → fhetch::Polynomial), pending output names for the
// next D2H, and the recording lifecycle flags.
//
// Mutex contract: most public methods take `mutex_` themselves; the
// `_locked` variants assume the caller has already taken it via an
// EpochSession and skip internal locking. Single std::mutex (not
// recursive) — the lock is held for tiny windows during instruction
// emission. The single-mutex design preserves IR ordering as the
// niobium target grows into multi-compute-unit configurations and
// FIDESlib's OpenMP regions fan out with num_threads > 1.
class EpochState {
  public:
    static EpochState &instance() noexcept;

    std::mutex &mutex() noexcept { return mutex_; }

    // ---- Public methods (take mutex_ internally) ----

    bool is_recording() noexcept;

    // Drop any polymap binding for `addr`. Called by the allocator paths
    // (H2D, D2D, memset, free) so the next compute call rebuilds a fresh
    // input from new shadow data.
    void invalidate(DevAddr addr) noexcept;

    // Materialize any pending compute output bound to `addr` so a
    // subsequent shadow read returns the post-replay value. No-op when
    // there is nothing to flush (not recording, or `addr` has no binding).
    // Combines the predicate and the materialization atomically under
    // mutex_ so a concurrent invalidate cannot slip between them.
    std::expected<void, HazeInternalError> flush_for_d2h(DevAddr addr) noexcept;

    void reset() noexcept;

    // ---- Locked methods (caller holds mutex_) ----

    // Initialise the compiler backend (idempotent) and start recording
    // if not already. EpochSession calls this after taking mutex_.
    void ensure_recording_locked();

    // Resolve `addr` to a polynomial. Returns a *copy* — required for
    // in-place compute operations (dst == src) where the destination
    // mutation must not invalidate the source. On first reference,
    // creates a Polynomial from the shadow buffer and tags it as input.
    std::expected<niobium::fhetch::Polynomial, HazeInternalError>
    lookup_or_create_locked(DevAddr addr);

    // Bind `addr` to `poly` (replacing any prior binding).
    void store_locked(DevAddr addr, niobium::fhetch::Polynomial poly) noexcept;

  private:
    EpochState() = default;
    EpochState(const EpochState &) = delete;
    EpochState &operator=(const EpochState &) = delete;

    // Internal helper: drain pending outputs through the backend. Caller
    // holds mutex_. Always resets epoch state at the end (cleared on both
    // success and failure to leave the next epoch with a clean slate).
    std::expected<void, HazeInternalError> do_materialize_locked();

    void clear_state_locked() noexcept;

    // mutex_ protects all state below. Lock order across HAZE: any caller
    // holding mutex_ may call into DeviceAllocator (epoch → allocator).
    // The reverse direction is forbidden; allocator-side code must never
    // call back into EpochState while holding the allocator lock.
    std::mutex mutex_;
    std::unordered_map<DevAddr, niobium::fhetch::Polynomial> poly_map_;
    std::unordered_map<DevAddr, std::string> pending_outputs_;
    uint64_t input_counter_ = 0;
    uint64_t output_counter_ = 0;
    bool recording_ = false;
};

inline EpochState &epoch() noexcept { return EpochState::instance(); }

// RAII helper used by the compute API entry points: ensures the backend
// is initialised, then takes the epoch mutex and ensures the recording
// is active for the lifetime of the session. Compute helpers
// (lookup_or_create_locked, store_locked) are safe to call while an
// EpochSession is in scope.
//
// `backend().ensure_initialized()` runs *before* the lock is taken so
// concurrent compute callers don't serialise on the (one-time) compiler
// init under the epoch mutex.
class EpochSession {
  public:
    EpochSession() : guard_(prepare_lock()) {
        EpochState::instance().ensure_recording_locked();
    }

    EpochSession(const EpochSession &) = delete;
    EpochSession &operator=(const EpochSession &) = delete;

  private:
    static std::unique_lock<std::mutex> prepare_lock() noexcept;

    std::unique_lock<std::mutex> guard_;
};

// D2H-side helper: copies bytes from the device shadow buffer to a host
// destination after triggering any pending materialization. Called by
// hazeMemcpy(D2H).
hazeError_t copy_to_host_with_flush(void *dst, DevAddr src, size_t count) noexcept;

} // namespace haze::detail
