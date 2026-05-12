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
#include "core/polynomial_io.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <ios>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

EpochState &EpochState::instance() noexcept {
    static EpochState inst;
    return inst;
}

void EpochState::ensure_recording_locked() {
    // EpochSession initializes the backend before locking; this defensive
    // check guards future code paths that might bypass EpochSession.
    if (!backend().is_initialized())
        return;
    if (!recording_) {
        // start_epoch() before start() memorizes the polynomial-ID base so
        // post-materialize resets snap back to it; without it, IDs drift.
        CompilerBackend::start_epoch();
        CompilerBackend::start_recording();
        recording_ = true;
    }
}

bool EpochState::is_recording() noexcept {
    HazeLockGuard lock(mutex_);
    return recording_;
}

void EpochState::invalidate(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    // pending_outputs_ is a subset of poly_map_; erase-on-miss is O(1), so
    // unconditional double-erase is simpler than tagging addrs by owner.
    poly_map_.erase(addr);
    pending_outputs_.erase(addr);
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = config().ring_dim();
    auto components = allocator().extract_polynomial_components(addr, ring_dim);

    fhetch::Polynomial poly;
    if (components) {
        poly = fhetch::Polynomial::from_data(std::move(*components), ring_dim,
                                             fhetch::Format::Evaluation);
    } else if (components.error() == HazeInternalError::NoData) {
        // Address allocated but never written: build a fhetch zero polynomial
        // so HAZE doesn't fabricate the bytes (matches FIDESlib's SPECIAL pattern).
        poly = fhetch::Polynomial::zeros(ring_dim);
    } else {
        return std::unexpected(components.error());
    }

    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);

    poly_map_.emplace(addr, poly);
    return poly;
}

void EpochState::store_compute_result_locked(DevAddr addr,
                                             niobium::fhetch::Polynomial poly) noexcept {
    poly_map_.insert_or_assign(addr, std::move(poly));
    // First store at `addr` reserves a pending output name; re-stores keep
    // it, and invalidate() wipes it so post-H2D/memset stores get a new one.
    if (!pending_outputs_.contains(addr)) {
        pending_outputs_.emplace(addr, "haze_out_" + std::to_string(output_counter_++));
    }
}

void EpochState::tag_mrp_input_if_new_locked(const std::string &name, const fhetch::MRP &mrp) {
    // Dedup so each name reaches fhetch exactly once.
    auto [it, inserted] = mrp_input_tagged_names_.insert(name);
    if (!inserted)
        return;
    fhetch::tag_input(*it, mrp);
}

std::expected<void, HazeInternalError>
EpochState::register_mrp_output_group_locked(std::span<const DevAddr> addrs,
                                             std::span<const uint64_t> moduli, std::string &&name) {
    // One device address per modulus; a mismatch is a programming bug in
    // the fan-out helper, surfaced rather than dropped silently.
    if (addrs.size() != moduli.size()) {
        std::ostringstream body;
        body << "register_mrp_output_group_locked('" << name << "'): addrs.size()=" << addrs.size()
             << " != moduli.size()=" << moduli.size();
        record_internal_error(HazeInternalError::MrpGroupAddrModuliMismatch, body.str().c_str());
        return std::unexpected(HazeInternalError::MrpGroupAddrModuliMismatch);
    }
    // try_emplace dedups re-registrations for the same op (e.g. in-place
    // hazeMulMrp called twice) since the leading-addr name is stable.
    auto [it, inserted] = pending_mrp_groups_.try_emplace(std::move(name));
    if (!inserted)
        return {};
    it->second.addrs.assign(addrs.begin(), addrs.end());
    it->second.moduli.assign(moduli.begin(), moduli.end());
    return {};
}

std::expected<void, HazeInternalError> EpochState::replay_and_populate() noexcept {
    HazeLockGuard lock(mutex_);
    if (!recording_) {
        return {}; // nothing to replay
    }

    // pending_outputs_ and poly_map_ stay in lockstep via store + invalidate,
    // so a missing binding here is a state-management bug, not recoverable.
    for (auto &[addr, name] : pending_outputs_) {
        auto it = poly_map_.find(addr);
        if (it == poly_map_.end()) {
            std::ostringstream body;
            body << "replay_and_populate: pending output '" << name << "' addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec << " missing from poly_map_";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        fhetch::tag_output(name, it->second);
    }

    // Also tag each MRP group as a fhetch MRP output so external callers
    // can pull the multi-residue view via fhetch::result(name, MRP&).
    for (const auto &[name, g] : pending_mrp_groups_) {
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g.addrs.size());
        for (size_t i = 0; i < g.addrs.size(); ++i) {
            auto it = poly_map_.find(g.addrs[i]);
            if (it == poly_map_.end()) {
                // Group registered but its poly_map_ binding was invalidated before replay.
                std::ostringstream body;
                body << "replay_and_populate: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g.addrs[i]) << std::dec << " missing from poly_map_";
                record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
                return std::unexpected(HazeInternalError::MissingPolyMapBinding);
            }
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(it->second, g.moduli[i]);
        }
        fhetch::tag_output(name, fhetch::MRP::from_pairs(pairs));
    }

    return do_materialize_locked();
}

std::expected<void, HazeInternalError> EpochState::do_materialize_locked() {
    if (!recording_) {
        return {};
    }

    // Step 1: write the trace. The replay_bridge post-recording hook
    // runs inside stop_epoch; we drain its failure flag right after.
    const bool stop_ok = CompilerBackend::stop_epoch();
    if (!stop_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::replay_and_populate (stop_epoch)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    if (hazeReplayBridgeTakeHookHadError() != 0) {
        clear_state_locked();
        record_internal_error(
            HazeInternalError::BridgeHookFailed,
            "post_recording_hook reported per-input/output failures (see prior log entries)");
        return std::unexpected(HazeInternalError::BridgeHookFailed);
    }

    // Step 2: dispatch replay. kLocalTarget runs the in-process simulator;
    // other targets spawn nbcc_fhetch_replay over HTTP — both produce
    // serialized_probes/<name>.ct for step 3 to read.
    const bool replay_ok = CompilerBackend::replay();
    if (!replay_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::replay_and_populate (replay)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Step 3: per-output shadow population. Any failure aborts the
    // epoch so a stale shadow can't surface as a silent wrong-value D2H.
    for (auto &[addr, name] : pending_outputs_) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::ostringstream body;
            body << "result('" << name << "') unavailable for addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            clear_state_locked();
            record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputMissing);
        }
        std::vector<uint64_t> values;
        if (!extract_polynomial_values(result_poly, name, values)) {
            std::ostringstream body;
            body << "failed to extract values for output '" << name << "' at addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            clear_state_locked();
            record_internal_error(HazeInternalError::BackendOutputDecodeFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
        }
        if (auto r = allocator().update_shadow(addr, std::move(values)); !r) {
            clear_state_locked();
            return std::unexpected(r.error());
        }
    }

    clear_state_locked(); // also clears captured inputs/outputs (E7).
    return {};
}

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    pending_outputs_.clear();
    pending_mrp_groups_.clear();
    mrp_input_tagged_names_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
    // Mirror clears to libnbfhetch so a failed materialise can't leak
    // captures into the next epoch; pairs with EpochSession's setup.
    niobium::compiler().clear_captured();
}

void EpochState::reset() noexcept {
    HazeLockGuard lock(mutex_);
    clear_state_locked();
}

HazeMutex &EpochSession::init_then_get_mutex() noexcept {
    // Run ensure_initialized() before grabbing the epoch lock so first-call
    // init doesn't serialize; failure surfaces later via is_initialized().
    [[maybe_unused]] const bool _ = backend().ensure_initialized();
    return epoch().mutex_;
}

hazeError_t copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    // D2H is the sole flush trigger: finalize, replay, and populate shadows
    // before reading. No-op when not recording, so H2D-then-D2H round-trips
    // and follow-up D2H reads in the same epoch are free.
    auto replay_result = epoch().replay_and_populate();
    if (!replay_result)
        return to_public_error(replay_result.error());
    return allocator().copy_to_host(dst, src, count);
}

} // namespace haze
