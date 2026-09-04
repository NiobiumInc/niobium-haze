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
#include "core/epoch.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"
#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "core/input_spill.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ios>
#include <niobium/fhetch_api.h>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

void EpochState::ensure_recording_locked() {
    if (recording_)
        return;
    // First compute brings the backend up here, under the lock, consuming the
    // result directly. A failed bring-up leaves recording_ false; the caller's
    // require_recording_locked() then names the reason at the ABI edge.
    if (!backend().ensure_initialized())
        return;
    // The root sits OUTSIDE the program dir (a sibling of it, not a child) so
    // fhebench's post-flush project rename cannot carry the store away, and an
    // input's on-disk residue survives a program dir change across epochs.
    auto dir = CompilerBackend::program_directory();
    if (!dir)
        return;
    if (!input_spill().activate(dir->parent_path() / "haze_input_spill"))
        return;
    // start_epoch() precedes start(); on any failure recording_ stays false so
    // require_recording_locked reports it. If start() fails after start_epoch()
    // opened the epoch, drop the partial captured state so a retry starts from a
    // clean registry instead of double-starting the epoch.
    if (CompilerBackend::start_epoch()) {
        if (CompilerBackend::start_recording())
            recording_ = true;
        else
            CompilerBackend::clear_captured();
    }
}

std::expected<void, HazeInternalError> EpochState::require_recording_locked() const noexcept {
    if (recording_)
        return {};
    // No explicit hazeConfigureDevice(): the frozen config isn't built, so report
    // that distinctly and before touching replay_config().
    if (!config_finalized()) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "require_recording_locked: hazeConfigureDevice() not called");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    record_internal_error(HazeInternalError::BackendInitFailed,
                          "require_recording_locked: backend init failed; compute cannot record");
    return std::unexpected(HazeInternalError::BackendInitFailed);
}

void EpochState::invalidate(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    // Drop any MRP group (and leading-addr names) so a recycled allocation
    // can't bind stale group state to a new polynomial at replay time.
    mrp_.invalidate(addr);
    // pending_outputs_ is a subset of poly_map_; erase-on-miss is O(1), so
    // unconditional double-erase is simpler than tagging addrs by owner.
    poly_map_.erase(addr);
    pending_outputs_.erase(addr);
    addr_modulus_.erase(addr);
    undeclared_uploads_.erase(addr);
    // Bytes are address-scoped now: an invalidated addr's on-disk residue, if any,
    // no longer describes anything live. Safe unconditionally -- bind_name snapshots
    // a tag's residue onto its own file at tag time, so freeing addr afterward (a
    // temporary key/ciphertext upload, freed right after the op that consumed it)
    // cannot perturb a still-pending tag's copy.
    if (input_spill().has(addr)) {
        // erase() already recorded the reason on failure; this void, noexcept caller
        // has no further channel to act on it.
        [[maybe_unused]] auto erased = input_spill().erase(addr);
    }
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = fhe_params().ring_dim();
    const std::string name = "haze_in_" + std::to_string(input_counter_);

    // Tracks whether THIS call promoted addr's shadow into the spill store, so a later
    // failure knows whether there is anything to undo: cross-epoch reuse (residue already
    // on disk) touches neither the shadow nor the store and needs no rollback.
    bool promoted_this_call = false;
    std::vector<uint64_t> residue;
    if (!input_spill().has(addr)) {
        // Fresh live-in: promote the shadow the same way an eager H2D tag would.
        auto components = allocator().extract_polynomial_components(addr, ring_dim);
        if (!components) {
            // An addr with neither shadow nor a spilled residue has no value to read;
            // translate NoData into the sharper SourceUnavailable.
            if (components.error() == HazeInternalError::NoData) {
                record_internal_error(
                    HazeInternalError::SourceUnavailable,
                    "lookup_or_create_locked: no shadow and no poly_map_ binding");
                return std::unexpected(HazeInternalError::SourceUnavailable);
            }
            return std::unexpected(components.error());
        }
        residue = *components;
        auto put = input_spill().put(addr, std::move(*components));
        if (!put) {
            // extract_polynomial_components() already evicted the shadow; restore it, since
            // the allocator's contract is that an error path never destroys the caller's bytes.
            auto restored = allocator().update_shadow(addr, std::move(residue));
            if (!restored) {
                record_internal_error(
                    HazeInternalError::SpillIoFailed,
                    "lookup_or_create_locked: rollback update_shadow failed "
                    "after put failure; addr left with neither shadow nor record");
            }
            return std::unexpected(put.error());
        }
        promoted_this_call = true;
    }
    // Cross-epoch reuse lands here directly: the residue is already on disk from an
    // earlier tag, so this recording only needs a fresh name binding for it.
    auto bound = input_spill().bind_name(name, {addr});
    if (!bound) {
        // A fresh promotion's put() already landed; undo it so addr ends up exactly as
        // before the call rather than leaving an orphaned store entry and evicted shadow.
        if (promoted_this_call) {
            [[maybe_unused]] auto erased = input_spill().erase(addr);
            auto restored = allocator().update_shadow(addr, std::move(residue));
            if (!restored) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "lookup_or_create_locked: rollback update_shadow failed "
                                      "after bind_name failure; addr left with neither shadow nor "
                                      "record");
            }
        }
        return std::unexpected(bound.error());
    }
    fhetch::Polynomial poly;
    try {
        poly = fhetch::Polynomial::zeros(ring_dim, fhetch::Format::Evaluation);
        fhetch::tag_input(name, poly);
        poly_map_.emplace(addr, poly);
    } catch (...) {
        if (promoted_this_call) {
            [[maybe_unused]] auto erased = input_spill().erase(addr);
            auto restored = allocator().update_shadow(addr, std::move(residue));
            if (!restored) {
                record_internal_error(HazeInternalError::SpillIoFailed,
                                      "lookup_or_create_locked: rollback update_shadow failed "
                                      "after tag_input threw; addr left with neither shadow nor "
                                      "record");
            }
        }
        throw;
    }
    // Counted only on success, matching tag_h2d_input_locked: a throw during emplace
    // must not advance input_counter_ past a name that never made it into poly_map_.
    input_counter_++;
    return poly;
}

bool EpochState::is_undeclared_upload_locked(DevAddr addr) const noexcept {
    return undeclared_uploads_.contains(addr);
}

void EpochState::store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly,
                                             uint64_t modulus) noexcept {
    poly_map_.insert_or_assign(addr, std::move(poly));
    undeclared_uploads_.erase(addr);
    // A no-modulus (kCopyModulus) result drops any stale entry so a later
    // copy/automorph can't recover a previous occupant's modulus here.
    if (modulus != kCopyModulus)
        addr_modulus_.insert_or_assign(addr, modulus);
    else
        addr_modulus_.erase(addr);
    // A compute result supersedes any spilled input residue this addr used to hold.
    // Safe unconditionally -- bind_name already snapshotted that residue onto its own
    // file for any tag still pending, so an in-place accumulation reusing addr as its
    // own destination cannot perturb what that tag reads at flush.
    if (input_spill().has(addr)) {
        // erase() already recorded the reason on failure; this void, noexcept caller
        // has no further channel to act on it.
        [[maybe_unused]] auto erased = input_spill().erase(addr);
    }
    // Evict stale shadow so a pre-flush D2H reports OutputNotFlushed, not the old bytes.
    allocator().evict_shadow(addr);
}

uint64_t EpochState::recorded_modulus_locked(DevAddr addr) const noexcept {
    auto it = addr_modulus_.find(addr);
    return it == addr_modulus_.end() ? kCopyModulus : it->second;
}

void EpochState::ensure_output_tag_locked(DevAddr addr) {
    if (!pending_outputs_.contains(addr))
        pending_outputs_.emplace(addr, "haze_out_" + std::to_string(output_counter_++));
}

std::expected<void, HazeInternalError> EpochState::tag_output_locked(DevAddr addr) {
    if (!poly_map_.contains(addr)) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "tag_output_locked: addr not bound in poly_map_");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }
    if (auto members = mrp_.mark_group_output(addr)) {
        for (DevAddr a : *members)
            ensure_output_tag_locked(a);
        return {};
    }
    ensure_output_tag_locked(addr);
    return {};
}

std::expected<void, HazeInternalError> EpochState::copy_result_locked(DevAddr dst, DevAddr src,
                                                                      uint64_t modulus) noexcept {
    // The op carries the COPY sentinel (the executor lowers ADDI imm=0 at modulus-table index
    // 0 as a register copy); the real modulus rides as metadata, recovered from the source
    // when the caller passed none.
    auto src_poly = lookup_or_create_locked(src);
    if (!src_poly)
        return std::unexpected(src_poly.error());
    if (modulus == kCopyModulus)
        modulus = recorded_modulus_locked(src);
    auto copy = fhetch::sr_addps(*src_poly, fhetch::Scalar::from_int(0), kCopyModulus);
    if (modulus != kCopyModulus) {
        // Bind the source too (a node only touched by copies would otherwise
        // stay sentinel-bound), and record src's modulus to match the binding
        // so a later copy/automorph of src recovers it.
        fhetch::bind_modulus(*src_poly, modulus);
        fhetch::bind_modulus(copy, modulus);
        addr_modulus_.insert_or_assign(src, modulus);
    }
    store_compute_result_locked(dst, std::move(copy), modulus);
    return {};
}

std::expected<void, HazeInternalError> EpochState::tag_h2d_input_locked(DevAddr addr) noexcept {
    // No recording (failed init): keep H2D as a plain shadow write and skip
    // the tag; a later compute fails at require_recording_locked.
    if (!recording_) {
        return {};
    }
    // recording_ implies a finalized FheParams (ring_dim set and validated at
    // build), so ring_dim is non-zero here; the read-back guard below covers the
    // remaining shadow-size invariant. Evicting: the shadow moves to the spill
    // store at tag time, so a pre-flush D2H is served from disk (EpochState::copy_to_host).
    const uint64_t ring_dim = fhe_params().ring_dim();
    auto components = allocator().extract_polynomial_components(addr, ring_dim);
    if (!components)
        return std::unexpected(components.error());
    auto residue = *components;
    auto put = input_spill().put(addr, std::move(*components));
    if (!put) {
        // extract_polynomial_components() already evicted the shadow; restore it, since
        // the allocator's contract is that an error path never destroys the caller's bytes.
        auto restored = allocator().update_shadow(addr, std::move(residue));
        if (!restored) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "tag_h2d_input_locked: rollback update_shadow failed after put "
                                  "failure; addr left with neither shadow nor record");
        }
        return std::unexpected(put.error());
    }
    const std::string name = "haze_in_" + std::to_string(input_counter_);
    auto bound = input_spill().bind_name(name, {addr});
    if (!bound) {
        // put() above already landed; undo it so addr ends up exactly as before the call
        // rather than an orphaned store entry sitting behind an evicted shadow.
        [[maybe_unused]] auto erased = input_spill().erase(addr);
        auto restored = allocator().update_shadow(addr, std::move(residue));
        if (!restored) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "tag_h2d_input_locked: rollback update_shadow failed after "
                                  "bind_name failure; addr left with neither shadow nor record");
        }
        return std::unexpected(bound.error());
    }
    // Same containment as tag_h2d_mrp_input_locked, and for a sharper reason
    // than the terminate: undeclared_uploads_ is recorded LAST, so a throw
    // there leaves an addr that really is a modulus-less upload but is not
    // marked as one. The refusal in build_mrp_locked would then stay silent and
    // a later multi-residue op would record this residue per-tower again -
    // exactly the shape the refusal exists to make loud.
    try {
        fhetch::Polynomial poly = fhetch::Polynomial::zeros(ring_dim, fhetch::Format::Evaluation);
        fhetch::tag_input(name, poly);
        // New H2D bytes overwrite the binding, drop any output tag and stale
        // modulus, and mark the addr as carrying bytes no prime was declared for
        // (MRP-group claims stay, but the query index's entry does not: this
        // upload, not the earlier MRP one, now governs these bytes).
        poly_map_.insert_or_assign(addr, std::move(poly));
        pending_outputs_.erase(addr);
        addr_modulus_.erase(addr);
        undeclared_uploads_.insert(addr);
        mrp_.forget_input_group(addr);
    } catch (...) {
        // put() and bind_name() both already committed; undo both before the epoch-wide
        // clear so addr itself is restored, not just left to a fresh epoch's bookkeeping.
        [[maybe_unused]] auto erased = input_spill().erase(addr);
        auto restored = allocator().update_shadow(addr, std::move(residue));
        if (!restored) {
            record_internal_error(HazeInternalError::SpillIoFailed,
                                  "tag_h2d_input_locked: rollback update_shadow failed after "
                                  "tag_input threw; addr left with neither shadow nor record");
        }
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "tag_h2d_input_locked threw; epoch state cleared");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    input_counter_++;
    return {};
}

std::expected<void, HazeInternalError>
EpochState::tag_h2d_mrp_input_locked(std::span<const DevAddr> addrs,
                                     std::span<const uint64_t> moduli) noexcept {
    // No recording (failed init): keep H2D as a plain shadow write and skip the
    // tag, matching tag_h2d_input_locked.
    if (!recording_) {
        return {};
    }
    const uint64_t ring_dim = fhe_params().ring_dim();

    // restore_shadows/erase_puts jointly implement the all-or-nothing contract for every
    // failure below, including in the extraction loop itself: only a PREFIX of addrs may
    // have had its shadow evicted or reached the spill store, so callers pass how many to
    // restore/erase. A restore failure is the one outcome leaving an addr with neither a
    // shadow nor a spill record, so it gets its own diagnostic beyond the store's own.
    std::vector<std::vector<uint64_t>> residues;
    residues.reserve(addrs.size());
    auto restore_shadows = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            auto restored = allocator().update_shadow(addrs[i], std::move(residues[i]));
            if (!restored) {
                std::ostringstream body;
                body << "tag_h2d_mrp_input_locked: rollback update_shadow failed for addr 0x"
                     << std::hex << to_uintptr(addrs[i])
                     << "; left with neither shadow nor spill record";
                record_internal_error(HazeInternalError::SpillIoFailed, body.str().c_str());
            }
        }
    };
    auto erase_puts = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (input_spill().has(addrs[i])) {
                [[maybe_unused]] auto erased = input_spill().erase(addrs[i]);
            }
        }
    };

    // Collect-then-commit: extract every residue into a local buffer FIRST, before any
    // is spilled, so a failure partway through never leaves some addrs already spilled
    // while others are not. extract_polynomial_components() evicts the shadow only on
    // success, so a failure here only has to restore what THIS loop already evicted.
    for (DevAddr a : addrs) {
        auto components = allocator().extract_polynomial_components(a, ring_dim);
        if (!components) {
            restore_shadows(residues.size());
            return std::unexpected(components.error());
        }
        residues.push_back(std::move(*components));
    }

    // Each put() takes a COPY of its residue (not a move) so the local `residues` stays
    // authoritative for rollback until the whole tag has committed.
    std::size_t put_count = 0;
    for (; put_count < addrs.size(); ++put_count) {
        auto put = input_spill().put(addrs[put_count], std::vector<uint64_t>(residues[put_count]));
        if (!put) {
            erase_puts(put_count);
            restore_shadows(addrs.size());
            return std::unexpected(put.error());
        }
    }

    // One entry per upload under a fresh name: every upload builds fresh fhetch
    // polynomials, so reusing a name keyed on the leading addr would leave a
    // re-upload's addresses bound by no .ids file. next_input_group_name() advances
    // MrpGroupRegistry::in_counter_ as an inseparable part of computing the name, so
    // unlike input_counter_ this counter cannot be deferred to success without splitting
    // that method's API; left as-is on a failure below (out of scope for this fix).
    const std::string name = mrp_.next_input_group_name();
    auto bound = input_spill().bind_name(name, std::vector<DevAddr>(addrs.begin(), addrs.end()));
    if (!bound) {
        erase_puts(addrs.size());
        restore_shadows(addrs.size());
        return std::unexpected(bound.error());
    }
    // from_pairs / tag_input / bind_modulus all allocate and none is declared
    // no-throw, so a bad_alloc would cross this noexcept boundary and terminate
    // under the lock. It would also leave a half-applied binding: the trace's
    // input entry names every residue, so an addr this loop never reached would
    // be re-tagged by a later compute and land in the project twice - the
    // duplication this entry shape exists to prevent. The epoch cannot be
    // repaired from here, so clear it and report, as finalize_guarded_locked does.
    // Scoped claim: shadows and addr-scoped spill records do come back, via
    // restore_shadows/erase_puts and clear_state_locked's input_spill().clear_names().
    // The vendor arena's tag_input(name, ...) call above is NOT undone -
    // CompilerBackend::clear_captured() does not reach fhetch's own input registry, only
    // reset_for_epoch() does, and that runs only after a SUCCESSFUL flush - so the arena
    // may retain this attempt's tagged input until the next flush or reset. A later flush
    // that reaches this stale entry finds no spill record behind its name and reports
    // that loudly as a missing spill record.
    try {
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(addrs.size());
        for (std::size_t i = 0; i < addrs.size(); ++i)
            pairs.emplace_back(fhetch::Polynomial::zeros(ring_dim, fhetch::Format::Evaluation),
                               moduli[i]);
        fhetch::tag_input(name, fhetch::MRP::from_pairs(pairs));
        for (std::size_t i = 0; i < addrs.size(); ++i) {
            // The caller declared this residue's prime, so bind it rather than
            // erasing as the modulus-less single-H2D path must.
            fhetch::bind_modulus(pairs[i].first, moduli[i]);
            poly_map_.insert_or_assign(addrs[i], pairs[i].first);
            pending_outputs_.erase(addrs[i]);
            addr_modulus_.insert_or_assign(addrs[i], moduli[i]);
            undeclared_uploads_.erase(addrs[i]);
        }
        // Last statement of the commit: every fallible step above has already
        // succeeded, so a throw here is rolled back by the catch below exactly like
        // the state it sits beside, and a successful return commits it alongside them.
        mrp_.record_input_group(addrs, name);
    } catch (...) {
        erase_puts(addrs.size());
        restore_shadows(addrs.size());
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "tag_h2d_mrp_input_locked threw; epoch state cleared");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    return {};
}

std::expected<void, HazeInternalError>
EpochState::record_mrp_group_locked(std::span<const DevAddr> addrs,
                                    std::span<const uint64_t> moduli, std::string &&name) {
    auto was_pending = mrp_.record_mrp_group(addrs, moduli, std::move(name));
    if (!was_pending)
        return std::unexpected(was_pending.error());
    // Replacing an already-pending group: every member needs an output tag
    // for flush-time shadow population (tagging is idempotent).
    if (*was_pending)
        for (DevAddr a : addrs)
            ensure_output_tag_locked(a);
    return {};
}

std::expected<void, HazeInternalError> EpochState::tag_pending_outputs_locked() {
    // pending_outputs_ and poly_map_ stay in lockstep via store + invalidate,
    // so a missing binding here is a state-management bug, not recoverable.
    for (auto &[addr, name] : pending_outputs_) {
        // A residue of a pending MRP group is carried by that group's entry
        // below; emitting an SRP probe too would record the address twice and
        // leave a reader to decide which one governs it. Membership is settled
        // only now: latest-write-wins eviction can drop an addr out of its group
        // after tag_output_locked ran, and such an addr keeps its SRP probe.
        if (mrp_.pending_group_for(addr) != nullptr)
            continue;
        auto it = poly_map_.find(addr);
        if (it == poly_map_.end()) {
            std::ostringstream body;
            body << "tag_pending_outputs_locked: pending output '" << name << "' addr 0x"
                 << std::hex << to_uintptr(addr) << std::dec << " missing from poly_map_";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        fhetch::tag_output(name, it->second);
    }

    // Tag each pending MRP group as a fhetch MRP output: the sole entry for its
    // residues, read back per-residue at materialize via fhetch::result(name,
    // MRP&); find() resolves the latest registration for the name.
    for (const auto &name : mrp_.pending_names()) {
        const auto *g = mrp_.find(name);
        if (g == nullptr) {
            // pending ⊆ known is a registry invariant; a miss is a bug.
            std::ostringstream body;
            body << "tag_pending_outputs_locked: pending MRP group '" << name
                 << "' missing from the registry";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g->addrs.size());
        for (size_t i = 0; i < g->addrs.size(); ++i) {
            auto it = poly_map_.find(g->addrs[i]);
            if (it == poly_map_.end()) {
                // Group registered but its poly_map_ binding was invalidated before materialize.
                std::ostringstream body;
                body << "tag_pending_outputs_locked: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g->addrs[i]) << std::dec << " missing from poly_map_";
                record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
                return std::unexpected(HazeInternalError::MissingPolyMapBinding);
            }
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(it->second, g->moduli[i]);
        }
        fhetch::tag_output(name, fhetch::MRP::from_pairs(pairs));
    }

    return {};
}

std::expected<void, HazeInternalError> EpochState::finalize_locked(bool run_replay) {
    if (!recording_) {
        return {};
    }

    // Nothing tagged is a TRUE no-op (recording, bindings, counters survive): the vendor
    // recorder's start() no-ops while running, so a half-clear would emit both epochs' nodes
    // into one trace on the next flush.
    if (pending_outputs_.empty() && !mrp_.has_pending()) {
        return {};
    }

    if (auto tagged = tag_pending_outputs_locked(); !tagged)
        return std::unexpected(tagged.error());

    // State always resets after materialization so the next epoch starts clean
    // on success or failure.
    auto materialized = materialize_epoch(run_replay);
    clear_state_locked();
    // Success only: after a failed stop the vendor recorder may still be open, and
    // releasing here would roll the address counter back under a live trace. The
    // registries stay until a recording actually restarts or the device resets.
    if (materialized.has_value())
        release_fhetch_registries_locked();
    return materialized;
}

std::expected<void, HazeInternalError> EpochState::replay_and_populate() noexcept {
    HazeLockGuard lock(mutex_);
    return finalize_guarded_locked(/*run_replay=*/true);
}

std::expected<void, HazeInternalError> EpochState::write_program() noexcept {
    HazeLockGuard lock(mutex_);
    return finalize_guarded_locked(/*run_replay=*/false);
}

std::expected<void, HazeInternalError> EpochState::finalize_guarded_locked(bool run_replay) {
    // Catch-all so a vendor throw (fhetch tag_output / MRP assembly) inside
    // the flush chain becomes an error, not std::terminate under the lock.
    try {
        return finalize_locked(run_replay);
    } catch (...) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "finalize threw; epoch state cleared");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
}

// Instance method (not static) so its HAZE_REQUIRES(mutex_) contract binds to this
// EpochState's own mutex_, even though the body touches no other member.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void EpochState::release_fhetch_registries_locked() noexcept {
    try {
        fhetch::reset_for_epoch();
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "release_fhetch_registries_locked");
    }
}

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    pending_outputs_.clear();
    addr_modulus_.clear();
    undeclared_uploads_.clear();
    mrp_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
    // Mirror clears to libnbfhetch so a failed materialise can't leak
    // captures into the next epoch; pairs with EpochSession's setup. Only the
    // per-recording name manifest clears here: spilled bytes are address-scoped,
    // not epoch-scoped, so a live input's residue survives into the next epoch.
    CompilerBackend::clear_captured();
    input_spill().clear_names();
}

std::string EpochState::mrp_group_name_locked(DevAddr leading) {
    return mrp_.group_name(leading);
}

std::expected<void, HazeInternalError> EpochState::tag_output(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    try {
        return tag_output_locked(addr);
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed, "tag_output threw");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
}

std::expected<std::string, HazeInternalError> EpochState::input_group_name(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    try {
        const std::string *name = mrp_.input_group_name(addr);
        if (name == nullptr) {
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "input_group_name: addr has no live input group binding");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        return *name;
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed, "input_group_name threw");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
}

void EpochState::reset() noexcept {
    HazeLockGuard lock(mutex_);
    clear_state_locked();
    // reset_for_epoch resets fhetch's recording registries and counters, none of
    // which the imminent compiler teardown needs. This control-plane call always
    // precedes CompilerBackend::reset_compiler() (DeviceState::reset()), which
    // discards the vendor recorder outright, so no later op can observe the
    // rolled-back address counter.
    release_fhetch_registries_locked();
}

std::expected<void, HazeInternalError> EpochState::copy_to_host(void *dst, DevAddr src,
                                                                size_t count) noexcept {
    HazeLockGuard lock(mutex_);
    // A live input address reads from the spill store instead of the (evicted) shadow.
    // A read error propagates as-is (never remapped).
    if (count != 0 && input_spill().has(src)) {
        return input_spill().read(src, std::span<std::byte>(static_cast<std::byte *>(dst), count));
    }
    return allocator().copy_to_host(dst, src, count);
}

HazeMutex &EpochSession::epoch_mutex() noexcept {
    return epoch().mutex_;
}

std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    return epoch().copy_to_host(dst, src, count);
}

std::expected<void, HazeInternalError> write_program() noexcept {
    return epoch().write_program();
}

std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept {
    return epoch().tag_output(addr);
}

std::expected<std::string, HazeInternalError> input_group_name(DevAddr addr) noexcept {
    return epoch().input_group_name(addr);
}

std::expected<void, HazeInternalError> flush() noexcept {
    return epoch().replay_and_populate();
}

std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept {
    // Validate dst liveness and exact-polynomial count up front (contract in epoch.hpp).
    if (auto live = allocator().require_allocated(dst); !live)
        return std::unexpected(live.error());
    if (count == 0)
        return {}; // zero-byte copy: validated success no-op, nothing recorded
    const size_t poly_bytes = allocator().polynomial_size();
    if (count > poly_bytes) {
        record_internal_error(HazeInternalError::PolySizeMismatch, "copy_device_to_device");
        return std::unexpected(HazeInternalError::PolySizeMismatch);
    }
    if (count < poly_bytes) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "copy_device_to_device: partial D2D not expressible in IR");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return std::unexpected(rec.error());
    return epoch().copy_result_locked(dst, src);
}

std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept {
    EpochSession session;
    return epoch().tag_h2d_input_locked(addr);
}

} // namespace haze
