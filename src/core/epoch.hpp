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

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <mutex>
#include <niobium/fhetch_api.h>
#include <string>
#include <unordered_map>

namespace haze {

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
//
// The mutex contract is enforced by clang's thread-safety analysis
// (haze_thread_safety.hpp). Each `_locked` method requires the caller
// to hold mutex_; each public method excludes it; each protected field
// is guarded by it. Building with -Wthread-safety promotes any
// violation to a compile-time error.
class EpochState {
  public:
    static EpochState &instance() noexcept;

    // The mutex itself. EpochSession is the canonical caller; other
    // callers take it via std::lock_guard (see is_recording, invalidate,
    // replay_and_populate, reset).
    HazeMutex &mutex() noexcept HAZE_RETURN_CAPABILITY(mutex_) { return mutex_; }

    // ---- Public methods (take mutex_ internally) ----

    bool is_recording() noexcept HAZE_EXCLUDES(mutex_);

    // Drop any polymap binding for `addr`. Called by the allocator paths
    // (H2D, D2D, memset, free) so the next compute call rebuilds a fresh
    // input from new shadow data.
    void invalidate(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // Finalize the current epoch: tag every bound compute result as a
    // fhetch output, write the .fhetch trace via stop_epoch(), then
    // dispatch replay (see hazeSetTarget docs for the three-tier table)
    // and populate the host shadow buffer for each output address with
    // the computed values.
    //
    // After this call returns, hazeMemcpy(D2H) on any bound output
    // address reads the materialized value from the shadow buffer.
    // No-op when not recording.
    std::expected<void, HazeInternalError> replay_and_populate() noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    // ---- Locked methods (caller holds mutex_) ----

    // Initialise the compiler backend (idempotent) and start recording
    // if not already. EpochSession calls this after taking mutex_.
    void ensure_recording_locked() HAZE_REQUIRES(mutex_);

    // Resolve `addr` to a polynomial. Returns a *copy* — required for
    // in-place compute operations (dst == src) where the destination
    // mutation must not invalidate the source. On first reference,
    // creates a Polynomial from the shadow buffer and tags it as input.
    std::expected<niobium::fhetch::Polynomial, HazeInternalError>
    lookup_or_create_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // Bind `addr` to `poly` (replacing any prior binding) and tag it as
    // a compute output — every store of a freshly computed polynomial
    // gets a pending-output name on first store. Re-stores at the same
    // addr keep their original name (insert-no-overwrite); an
    // intervening invalidate() wipes pending_outputs_, so a fresh store
    // after H2D / memset gets a new name.
    void store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly) noexcept
        HAZE_REQUIRES(mutex_);

  private:
    EpochState() = default;
    EpochState(const EpochState &) = delete;
    EpochState &operator=(const EpochState &) = delete;

    // Internal helper: drain pending outputs through the backend. Caller
    // holds mutex_. Always resets epoch state at the end (cleared on both
    // success and failure to leave the next epoch with a clean slate).
    std::expected<void, HazeInternalError> do_materialize_locked() HAZE_REQUIRES(mutex_);

    void clear_state_locked() noexcept HAZE_REQUIRES(mutex_);

    // mutex_ protects all state below. Lock order across HAZE: any caller
    // holding mutex_ may call into DeviceAllocator (epoch → allocator).
    // The reverse direction is forbidden; allocator-side code must never
    // call back into EpochState while holding the allocator lock.
    HazeMutex mutex_;
    // Authoritative bindings for every poly in flight this epoch. Inputs
    // (lookup_or_create_locked) and outputs (store_compute_result_locked)
    // both land here; the corresponding pending_outputs_ entry, present
    // iff the binding is an output, names it for fhetch::tag_output.
    std::unordered_map<DevAddr, niobium::fhetch::Polynomial> poly_map_ HAZE_GUARDED_BY(mutex_);
    std::unordered_map<DevAddr, std::string> pending_outputs_ HAZE_GUARDED_BY(mutex_);
    uint64_t input_counter_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t output_counter_ HAZE_GUARDED_BY(mutex_) = 0;
    bool recording_ HAZE_GUARDED_BY(mutex_) = false;

    // Friendship so EpochSession's ACQUIRE/RELEASE attributes can name
    // mutex_ directly — TSA needs the capability expression in the
    // attribute to match the one used by the _locked methods.
    friend class EpochSession;
};

inline EpochState &epoch() noexcept {
    return EpochState::instance();
}

// RAII helper used by the compute API entry points: ensures the backend
// is initialised, then takes the epoch mutex and ensures the recording
// is active for the lifetime of the session. Compute helpers
// (lookup_or_create_locked, store_compute_result_locked) are safe to call while an
// EpochSession is in scope.
//
// `backend().ensure_initialized()` runs *before* the lock is taken so
// concurrent compute callers don't serialise on the (one-time) compiler
// init under the epoch mutex.
class HAZE_SCOPED_CAPABILITY EpochSession {
  public:
    EpochSession() HAZE_ACQUIRE(epoch().mutex_) : guard_(init_then_get_mutex()) {
        epoch().ensure_recording_locked();
    }
    // Defaulted destructor: the actual unlock runs in HazeLockGuard's
    // destructor (member guard_). HAZE_RELEASE here just declares the
    // capability hand-off to TSA; no body work is needed.
    ~EpochSession() HAZE_RELEASE() = default;

    EpochSession(const EpochSession &) = delete;
    EpochSession &operator=(const EpochSession &) = delete;

  private:
    // Side-effect helper: runs backend().ensure_initialized() (which has
    // its own internal init mutex) BEFORE returning the epoch mutex
    // reference. Sequencing matters — first-call compiler init must not
    // happen under the epoch lock or concurrent callers serialize on it.
    // Returning by reference to the very mutex that guard_ then locks
    // lets TSA verify the ACQUIRE annotation on the constructor.
    static HazeMutex &init_then_get_mutex() noexcept HAZE_RETURN_CAPABILITY(epoch().mutex_);

    HazeLockGuard guard_;
};

// D2H-side helper: copies bytes from the device shadow buffer to a host
// destination. Plain shadow read — does NOT trigger any recording
// finalization or replay. Callers that need post-compute values must
// invoke hazeReplay() (haze::epoch().replay_and_populate()) first to
// materialize results into the shadow buffer.
hazeError_t copy_to_host(void *dst, DevAddr src, size_t count) noexcept;

} // namespace haze
