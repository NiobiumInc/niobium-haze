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
#include <niobium/fhetch_api.h>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

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

    // Drop any polymap binding for `addr`. Called by allocator paths
    // (H2D, D2D, memset, free) so the next compute rebuilds from shadow.
    void invalidate(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    // Finalize the epoch: tag outputs, write the trace, dispatch replay,
    // populate shadow buffers. No-op when not recording.
    std::expected<void, HazeInternalError> replay_and_populate() noexcept HAZE_EXCLUDES(mutex_);

    // Finalize the epoch and write the project directory (trace + inputs +
    // templates + cryptocontext) WITHOUT dispatching replay or populating
    // shadow buffers. Backs hazeWriteProgram() for the record-here /
    // replay-elsewhere (e.g. FPGA) flow. No-op when not recording.
    std::expected<void, HazeInternalError> materialize_only() noexcept HAZE_EXCLUDES(mutex_);

    // Declare `addr` an output of the active recording (backs hazeTagOutput).
    // Takes mutex_ but does NOT start a recording: tagging with nothing
    // recorded (empty poly_map_) returns SourceUnavailable.
    std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept HAZE_EXCLUDES(mutex_);

    void reset() noexcept HAZE_EXCLUDES(mutex_);

    // ---- Locked methods (caller holds mutex_) ----

    // Initialise the compiler backend (idempotent) and start recording.
    void ensure_recording_locked() HAZE_REQUIRES(mutex_);

    // Resolve `addr`; returns a copy so in-place compute doesn't invalidate
    // the source. On first reference, builds from shadow and tags as input.
    std::expected<niobium::fhetch::Polynomial, HazeInternalError>
    lookup_or_create_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // Bind `addr` to `poly`. Output-hood is declared explicitly via
    // tag_output_locked, not inferred from being computed.
    void store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly) noexcept
        HAZE_REQUIRES(mutex_);

    // Declare `addr` an output (idempotent); it must name a value bound in
    // poly_map_ or it is a caller error. Tagging any residue of a known MRP
    // group tags the whole ciphertext and promotes the group for emission.
    std::expected<void, HazeInternalError> tag_output_locked(DevAddr addr) HAZE_REQUIRES(mutex_);

    // Record a pass-through copy dst <- src under the COPY_MODULUS sentinel.
    // A copy is modulus-agnostic and H2D eager-tags inputs, so the op needn't
    // carry a real prime; shared by the SRP and MRP D2D paths.
    std::expected<void, HazeInternalError> copy_result_locked(DevAddr dst, DevAddr src) noexcept
        HAZE_REQUIRES(mutex_);

    // Eagerly tag the H2D'd shadow bytes at `addr` as a fhetch input.
    // Builds the Polynomial via a non-evicting read (shadow stays intact
    // for subsequent compute-free D2H), calls `tag_input`, and binds it
    // in poly_map_. Returns an internal error if the H2D post-conditions
    // (ring_dim set, shadow populated) are violated.
    std::expected<void, HazeInternalError> tag_h2d_input_locked(DevAddr addr) noexcept
        HAZE_REQUIRES(mutex_);

    // Register an MRP-shaped grouping so replay can emit a single
    // fhetch::tag_output(name, MRP); deduped by name on re-registration.
    std::expected<void, HazeInternalError>
    register_mrp_output_group_locked(std::span<const DevAddr> addrs,
                                     std::span<const uint64_t> moduli, std::string &&name)
        HAZE_REQUIRES(mutex_);

    // Pass-through to fhetch::tag_input(name, MRP), deduped by name.
    void tag_mrp_input_if_new_locked(const std::string &name, const niobium::fhetch::MRP &mrp)
        HAZE_REQUIRES(mutex_);

    EpochState(const EpochState &) = delete;
    EpochState &operator=(const EpochState &) = delete;

  private:
    EpochState() = default;

    // Tag pending SRP + MRP outputs for fhetch. Shared by replay_and_populate
    // and materialize_only; returns the binding error without clearing state.
    std::expected<void, HazeInternalError> tag_pending_outputs_locked() HAZE_REQUIRES(mutex_);

    // Shared finalize entry: early-out when idle, tag outputs, then materialize.
    // run_replay=false stops after the trace is written (hazeWriteProgram).
    std::expected<void, HazeInternalError> finalize_locked(bool run_replay) HAZE_REQUIRES(mutex_);

    // Write the trace (step 1) and, when run_replay, dispatch replay + populate
    // shadows (steps 2-3). Always resets state at the end so the next epoch
    // starts clean on success or failure.
    std::expected<void, HazeInternalError> do_materialize_locked(bool run_replay)
        HAZE_REQUIRES(mutex_);

    void clear_state_locked() noexcept HAZE_REQUIRES(mutex_);

    // Lock order: epoch → allocator only. Allocator-side code must
    // never call back into EpochState while holding its own lock.
    HazeMutex mutex_;
    // Every poly in flight this epoch (inputs and outputs land here).
    // pending_outputs_ is the addr-keyed subset that names the outputs.
    std::unordered_map<DevAddr, niobium::fhetch::Polynomial> poly_map_ HAZE_GUARDED_BY(mutex_);
    std::unordered_map<DevAddr, std::string> pending_outputs_ HAZE_GUARDED_BY(mutex_);
    // MRP-shaped output groupings, keyed by name so re-registration of
    // the same op (same dst[0] → same name) is a free dedup.
    struct PendingMrpGroup {
        std::vector<DevAddr> addrs;   // residue addrs in encounter order
        std::vector<uint64_t> moduli; // base[i] paired with addrs[i]
    };
    // Every MRP group seen this epoch (auto-registered by the MRP ops). Holds
    // the group structure so tag_output_locked can expand a tagged residue to
    // the whole ciphertext; membership alone does not materialize anything.
    std::unordered_map<std::string, PendingMrpGroup> known_mrp_groups_ HAZE_GUARDED_BY(mutex_);
    // The explicitly-tagged subset of known_mrp_groups_ that gets emitted as a
    // fhetch MRP output at materialize time.
    std::unordered_map<std::string, PendingMrpGroup> pending_mrp_groups_ HAZE_GUARDED_BY(mutex_);
    // Reverse index addr → group names, kept in lockstep with
    // known_mrp_groups_ so invalidate() drops stale registrations in O(group_size).
    std::unordered_map<DevAddr, std::unordered_set<std::string>>
        addr_to_mrp_groups_ HAZE_GUARDED_BY(mutex_);
    // Dedup set for MRP input tags; reset by clear_state_locked.
    std::unordered_set<std::string> mrp_input_tagged_names_ HAZE_GUARDED_BY(mutex_);
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

// Pure shadow read (backs hazeMemcpy D2H): the recording must already have run
// via flush(); an address with no materialized bytes reads as OutputNotFlushed.
// Does not finalize anything. The api/ shim translates the error at the C ABI edge.
std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept;

// Finalize an active recording by writing the project directory only — no
// replay, no shadow population. Peer of copy_to_host for the hazeWriteProgram
// path; no-op when not recording. The api/ shim translates the error.
std::expected<void, HazeInternalError> write_program() noexcept;

// Declare a device address an output of the current recording (backs
// hazeTagOutput). Does NOT start a recording — tagging with nothing recorded
// returns SourceUnavailable. The api/ shim translates the error at the C ABI edge.
std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept;

// Execute the recorded program: finalize, dispatch replay, and populate the
// shadow buffers for the tagged outputs (backs hazeFlush). No-op when not
// recording. The api/ shim translates the error.
std::expected<void, HazeInternalError> flush() noexcept;

// D2D as a recorded copy: promotes `src` if needed (via
// `lookup_or_create_locked`), emits a pass-through fhetch IR node, and
// binds `dst` to the result. Always starts an epoch — there is no
// pre-recording byte-copy escape hatch. Returns the internal error type
// so the api/ shim does the public-code translation at the C ABI edge.
std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept;

// H2D-time eager-tag: register the H2D'd buffer at `addr` as a fhetch
// input so subsequent compute / D2D ops see a tagged polynomial instead
// of a never-touched shadow buffer.
std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept;

} // namespace haze
