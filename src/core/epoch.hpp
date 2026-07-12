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
#include "core/mrp_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

// fhetch's copy sentinel (TraceWriter COPY_MODULUS_VALUE); doubles as the
// "modulus unknown" marker for the addr->modulus tracking below.
inline constexpr uint64_t kCopyModulus = 0xFFFFFFFFFFFFFFFFULL;

class EpochState;
EpochState &epoch() noexcept; // inline definition after the class

// Singleton tracking the polymap, pending outputs, and recording flag for
// the active epoch; replay_and_populate() drains it at flush time. Public
// methods take mutex_; _locked variants require it held via EpochSession
// (enforced by clang -Wthread-safety).
class EpochState {
  public:
    static EpochState &instance() noexcept;

    // The mutex itself; EpochSession is the canonical acquirer.
    HazeMutex &mutex() noexcept HAZE_RETURN_CAPABILITY(mutex_) { return mutex_; }

    // ---- Public methods (take mutex_ internally) ----

    // Drop any binding for `addr` so the next read rebuilds from shadow. Called
    // on memset and free; H2D replaces the binding in place via tag_h2d_input_locked
    // and D2D binds via copy_result_locked, so neither routes through here.
    void invalidate(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // Finalize the epoch: tag outputs, write the trace, dispatch replay,
    // populate shadow buffers; a TRUE no-op when not recording or nothing
    // is tagged (the open recording and bindings survive).
    std::expected<void, HazeInternalError> replay_and_populate() noexcept HAZE_EXCLUDES(mutex_);

    // Finalize and write the program directory WITHOUT replay/population
    // (backs hazeWriteProgram); same true-no-op rule as replay_and_populate.
    std::expected<void, HazeInternalError> write_program() noexcept HAZE_EXCLUDES(mutex_);

    // Declare `addr` an output of the active recording (backs hazeTagOutput).
    // Takes mutex_ but does NOT start a recording: tagging with nothing
    // recorded (empty poly_map_) returns SourceUnavailable.
    std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    // ---- Locked methods (caller holds mutex_) ----

    // Initialise the compiler backend (idempotent) and start recording.
    void ensure_recording_locked() HAZE_REQUIRES(mutex_);

    // Compute-prelude gate: UnsupportedDataFormat for the
    // montgomery/bit-reversal-on-local refusal, BackendInitFailed otherwise.
    std::expected<void, HazeInternalError> require_recording_locked() const noexcept
        HAZE_REQUIRES(mutex_);

    // Resolve `addr`; returns a copy so in-place compute doesn't invalidate
    // the source. On first reference, builds from shadow and tags as input.
    std::expected<niobium::fhetch::Polynomial, HazeInternalError>
    lookup_or_create_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // True if `addr` is a live-in input (H2D upload or fresh shadow read), as
    // opposed to a value the trace produces (compute result / D2D copy).
    bool is_input_locked(DevAddr addr) const noexcept HAZE_REQUIRES(mutex_);

    // Bind `addr` to `poly` (output-hood stays explicit via tag_output_locked);
    // `modulus` records the residue's real modulus (kCopyModulus = unknown)
    // for later copy/automorph recovery, and any stale shadow at `addr` is
    // evicted so a pre-flush D2H reports OutputNotFlushed.
    void store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly,
                                     uint64_t modulus = kCopyModulus) noexcept
        HAZE_REQUIRES(mutex_);

    // Real modulus last recorded for `addr` by a modulus-carrying op, or
    // kCopyModulus if none (raw input, or a result whose op had no modulus).
    uint64_t recorded_modulus_locked(DevAddr addr) const noexcept HAZE_REQUIRES(mutex_);

    // Declare `addr` an output (idempotent); it must name a value bound in
    // poly_map_ or it is a caller error. Tagging any residue of a known MRP
    // group tags the whole ciphertext and promotes the group for emission.
    std::expected<void, HazeInternalError> tag_output_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // Record a pass-through copy dst <- src. Pass the residue's real modulus
    // when the caller knows it (MRP D2D has base[i]); otherwise the modulus is
    // recovered from the source's recorded_modulus_locked, so a copy of a
    // compute-produced (or otherwise modulus-bound) SRP value is still
    // probe-serializable on transport. Only a copy of a never-modulus-bound
    // address (raw opaque H2D buffer) stays sentinel-only.
    std::expected<void, HazeInternalError>
    copy_result_locked(DevAddr dst, DevAddr src, uint64_t modulus = kCopyModulus) noexcept
        HAZE_REQUIRES(mutex_);

    // Eagerly tag the H2D'd shadow bytes at `addr` as a fhetch input.
    // Builds the Polynomial via a non-evicting read (shadow stays intact
    // for subsequent compute-free D2H), calls `tag_input`, and binds it
    // in poly_map_. Returns an internal error if the H2D post-conditions
    // (ring_dim set, shadow populated) are violated.
    std::expected<void, HazeInternalError> tag_h2d_input_locked(DevAddr addr) noexcept
        HAZE_REQUIRES(mutex_);

    // Register an MRP-shaped grouping so replay can emit a single
    // fhetch::tag_output(name, MRP). Latest-write-wins on re-registration:
    // identical membership is a no-op, anything else replaces/evicts (see the
    // implementation for the full semantics).
    std::expected<void, HazeInternalError> record_mrp_group_locked(std::span<const DevAddr> addrs,
                                                                   std::span<const uint64_t> moduli,
                                                                   std::string &&name)
        HAZE_REQUIRES(mutex_);

    // Pass-through to fhetch::tag_input(name, MRP), deduped by name. Unlike
    // output groups (latest-write-wins, see record_mrp_group_locked),
    // input tags reach fhetch immediately and keep first-wins dedup —
    // input-side dst[0] reuse staleness is a known, separate limitation.
    void tag_mrp_input_if_new_locked(const std::string &name, const niobium::fhetch::MRP &mrp)
        HAZE_REQUIRES(mutex_);

    // Stable counter name for the MRP group led by `leading` ("haze_mrp_in_N"
    // / "haze_mrp_out_N"): same leading addr -> same name within an epoch;
    // invalidate() drops it so a recycled allocation gets a fresh name.
    std::string mrp_group_name_locked(bool output, DevAddr leading) HAZE_REQUIRES(mutex_);

    EpochState(const EpochState &) = delete;
    EpochState &operator=(const EpochState &) = delete;

  private:
    EpochState() = default;

    // Tag pending SRP + MRP outputs for fhetch. Shared by replay_and_populate
    // and write_program; returns the binding error without clearing state.
    std::expected<void, HazeInternalError> tag_pending_outputs_locked() HAZE_REQUIRES(mutex_);

    // Shared finalize entry: true no-op when idle or nothing tagged, else
    // tag outputs and materialize (run_replay=false stops after the trace).
    std::expected<void, HazeInternalError> finalize_locked(bool run_replay) HAZE_REQUIRES(mutex_);

    // finalize_locked behind a catch-all: a vendor throw becomes an error at
    // the C ABI instead of terminating through the noexcept flush entries.
    std::expected<void, HazeInternalError> finalize_guarded_locked(bool run_replay)
        HAZE_REQUIRES(mutex_);

    // Backend/bridge orchestration for a finalized epoch (stop, replay,
    // populate shadows); defined in core/materialize.cpp to keep OpenFHE /
    // replay-bridge includes out of epoch.cpp. Never clears state — the
    // caller does that after it returns.
    std::expected<void, HazeInternalError> materialize_epoch(bool run_replay) HAZE_REQUIRES(mutex_);

    void clear_state_locked() noexcept HAZE_REQUIRES(mutex_);

    // Name `addr` as a pending output if it isn't one already.
    void ensure_output_tag_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // Lock order: see the canonical DAG in common/thread_safety.hpp
    // (epoch → {config, allocator}). Allocator-side code must never
    // call back into EpochState while holding its own lock.
    HazeMutex mutex_;
    // Every poly in flight this epoch (inputs and outputs land here).
    // pending_outputs_ is the addr-keyed subset that names the outputs.
    std::unordered_map<DevAddr, niobium::fhetch::Polynomial> poly_map_ HAZE_GUARDED_BY(mutex_);
    std::unordered_map<DevAddr, std::string> pending_outputs_ HAZE_GUARDED_BY(mutex_);
    // Subset of poly_map_ addrs that are live-in inputs (H2D upload / fresh
    // shadow read), kept in lockstep with poly_map_; backs is_input_locked.
    std::unordered_set<DevAddr> input_addrs_ HAZE_GUARDED_BY(mutex_);
    // addr -> real modulus from the last modulus-carrying op that wrote it.
    // Kept in lockstep with poly_map_; cleared per epoch, dropped on invalidate.
    std::unordered_map<DevAddr, uint64_t> addr_modulus_ HAZE_GUARDED_BY(mutex_);
    // MRP group bookkeeping (membership, pending export, stable names);
    // invariants documented on the class.
    MrpGroupRegistry mrp_ HAZE_GUARDED_BY(mutex_);
    uint64_t input_counter_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t output_counter_ HAZE_GUARDED_BY(mutex_) = 0;
    bool recording_ HAZE_GUARDED_BY(mutex_) = false;

    // Friend so EpochSession's ACQUIRE/RELEASE attributes can name mutex_.
    friend class EpochSession;
};

inline EpochState &epoch() noexcept {
    return EpochState::instance();
}

// RAII guard for compute entry points: brings up the backend, takes the
// epoch mutex, and enters recording mode for the session lifetime.
// Backend init runs before the lock so concurrent callers don't
// serialize on the one-time setup.
class HAZE_SCOPED_CAPABILITY EpochSession {
  public:
    EpochSession() HAZE_ACQUIRE(epoch().mutex_) : guard_(init_then_get_mutex()) {
        epoch().ensure_recording_locked();
    }
    // The unlock runs in guard_'s destructor; HAZE_RELEASE just
    // declares the capability hand-off to TSA.
    ~EpochSession() HAZE_RELEASE() = default;

    EpochSession(const EpochSession &) = delete;
    EpochSession &operator=(const EpochSession &) = delete;

  private:
    // Runs backend().ensure_initialized() before returning the mutex
    // reference, so first-call compiler init isn't serialized under
    // the epoch lock.
    static HazeMutex &init_then_get_mutex() noexcept HAZE_RETURN_CAPABILITY(epoch().mutex_);

    HazeLockGuard guard_;
};

// haze:: entry points for the api/ shims, which translate the returned
// HazeInternalError at the C ABI edge. These forward to the EpochState members
// above (or the allocator), which carry the detailed contract.

// Pure shadow read backing hazeMemcpy D2H; unmaterialized bytes read as
// OutputNotFlushed.
std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept;

// Backs hazeWriteProgram (EpochState::write_program).
std::expected<void, HazeInternalError> write_program() noexcept;

// Backs hazeTagOutput (EpochState::tag_output).
std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept;

// Backs hazeFlush (EpochState::replay_and_populate).
std::expected<void, HazeInternalError> flush() noexcept;

// D2D as a recorded pass-through copy (always starts an epoch): `dst` must
// be live and `count` must equal the polynomial size — partial D2D is
// unexpressible in the IR (InvalidArgument; oversized is PolySizeMismatch).
std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept;

// H2D-time eager-tag: register the H2D'd buffer at `addr` as a fhetch input
// (EpochState::tag_h2d_input_locked).
std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept;

} // namespace haze
