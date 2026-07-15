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
    // Configured but recording refused: name the montgomery/bit-reversal-on-local
    // case distinctly from a backend-init failure.
    const ReplayConfig &rc = replay_config();
    if ((rc.montgomery() || rc.bit_reversal()) && rc.target_is_local()) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "require_recording_locked (montgomery/bit_reversal require a "
                              "transport target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
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
    input_addrs_.erase(addr);
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = fhe_params().ring_dim();
    auto components = allocator().extract_polynomial_components(addr, ring_dim);
    if (!components) {
        // Compute / D2D on an addr with neither shadow data nor a
        // poly_map_ binding is undefined under the record-and-replay
        // model — there's no value to read. Translate NoData into the
        // sharper SourceUnavailable; pass other errors through.
        if (components.error() == HazeInternalError::NoData) {
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "lookup_or_create_locked: no shadow and no poly_map_ binding");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        return std::unexpected(components.error());
    }
    fhetch::Polynomial poly =
        fhetch::Polynomial::from_data(std::move(*components), ring_dim, fhetch::Format::Evaluation);
    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);
    poly_map_.emplace(addr, poly);
    input_addrs_.insert(addr);
    return poly;
}

bool EpochState::is_input_locked(DevAddr addr) const noexcept {
    return input_addrs_.contains(addr);
}

void EpochState::store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly,
                                             uint64_t modulus) noexcept {
    poly_map_.insert_or_assign(addr, std::move(poly));
    // This addr now holds a trace-produced value, not a live-in input.
    input_addrs_.erase(addr);
    // A no-modulus (kCopyModulus) result drops any stale entry so a later
    // copy/automorph can't recover a previous occupant's modulus here.
    if (modulus != kCopyModulus)
        addr_modulus_.insert_or_assign(addr, modulus);
    else
        addr_modulus_.erase(addr);
    // Drop stale shadow bytes so a pre-flush D2H errors with
    // OutputNotFlushed instead of returning the old bytes.
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
    // An MRP residue tags every residue of its group and promotes the group.
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
    // The op carries the COPY sentinel (the executor lowers ADDI imm=0 at
    // modulus-table index 0 as a register copy); the real modulus rides as
    // metadata. Recover it from the source when the caller passed none.
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
    // remaining shadow-size invariant.
    const uint64_t ring_dim = fhe_params().ring_dim();
    auto components = allocator().read_polynomial_components(addr, ring_dim);
    if (!components)
        return std::unexpected(components.error());
    fhetch::Polynomial poly =
        fhetch::Polynomial::from_data(std::move(*components), ring_dim, fhetch::Format::Evaluation);
    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);
    // New H2D bytes overwrite the binding, drop any output tag, and reclassify
    // the addr as a live-in input (MRP-group claims stay).
    poly_map_.insert_or_assign(addr, std::move(poly));
    pending_outputs_.erase(addr);
    input_addrs_.insert(addr);
    return {};
}

void EpochState::tag_mrp_input_if_new_locked(const std::string &name, const fhetch::MRP &mrp) {
    // Dedup so each name reaches fhetch exactly once.
    if (mrp_.mark_input_tagged(name))
        fhetch::tag_input(name, mrp);
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

    // Also tag each pending MRP group as a fhetch MRP output so external
    // callers can pull the multi-residue view via fhetch::result(name, MRP&);
    // find() resolves the latest registration for the name.
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
        return {}; // nothing to finalize
    }

    // Nothing tagged: TRUE no-op (recording, bindings, counters survive).
    // A half-clear desyncs haze from the vendor recorder — its start()
    // no-ops while running, so the next flush would emit both epochs'
    // nodes into one trace.
    if (pending_outputs_.empty() && !mrp_.has_pending()) {
        return {};
    }

    if (auto tagged = tag_pending_outputs_locked(); !tagged)
        return std::unexpected(tagged.error());

    // State always resets after materialization so the next epoch starts clean
    // on success or failure.
    auto materialized = materialize_epoch(run_replay);
    clear_state_locked();
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

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    pending_outputs_.clear();
    addr_modulus_.clear();
    input_addrs_.clear();
    mrp_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
    // Mirror clears to libnbfhetch so a failed materialise can't leak
    // captures into the next epoch; pairs with EpochSession's setup.
    CompilerBackend::clear_captured();
}

std::string EpochState::mrp_group_name_locked(bool output, DevAddr leading) {
    return mrp_.group_name(output, leading);
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

void EpochState::reset() noexcept {
    HazeLockGuard lock(mutex_);
    clear_state_locked();
}

HazeMutex &EpochSession::epoch_mutex() noexcept {
    return epoch().mutex_;
}

std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    return allocator().copy_to_host(dst, src, count);
}

std::expected<void, HazeInternalError> write_program() noexcept {
    return epoch().write_program();
}

std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept {
    return epoch().tag_output(addr);
}

std::expected<void, HazeInternalError> flush() noexcept {
    // The montgomery/bit-reversal-on-local refusal is enforced at bring-up
    // against the frozen replay config; the format can't change afterwards, so
    // no flush-time re-check is needed.
    return epoch().replay_and_populate();
}

std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept {
    // Recorded pass-through copy of a whole polynomial: validate dst
    // liveness and exact-polynomial count up front (contract in epoch.hpp).
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
