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

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/replay_bridge.h>
#include <ios>
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
    // On any failure recording_ stays false and require_recording_locked
    // reports it; start_epoch() before start() keeps polynomial IDs stable.
    if (backend().is_initialized() && !recording_) {
        recording_ = CompilerBackend::start_epoch() && CompilerBackend::start_recording();
    }
}

std::expected<void, HazeInternalError> EpochState::require_recording_locked() const noexcept {
    if (recording_)
        return {};
    // Name the user-actionable refusal (montgomery/bit-reversal on local)
    // distinctly from a backend-init failure.
    if ((config().montgomery() || config().bit_reversal()) && config().target() == kLocalTarget) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "require_recording_locked (montgomery/bit_reversal require a "
                              "transport target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
    }
    record_internal_error(HazeInternalError::BackendInitFailed,
                          "require_recording_locked: backend init failed; compute cannot record");
    return std::unexpected(HazeInternalError::BackendInitFailed);
}

void EpochState::evict_mrp_group_locked(const std::string &name) noexcept {
    auto group_it = known_mrp_groups_.find(name);
    if (group_it == known_mrp_groups_.end())
        return;
    for (DevAddr member : group_it->second.addrs) {
        auto o = addr_to_mrp_groups_.find(member);
        if (o == addr_to_mrp_groups_.end())
            continue;
        o->second.erase(name);
        if (o->second.empty())
            addr_to_mrp_groups_.erase(o);
    }
    known_mrp_groups_.erase(group_it);
    pending_mrp_groups_.erase(name);
}

void EpochState::invalidate(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    // Drop any MRP group that names addr so a recycled allocation can't
    // bind a stale group entry to a new polynomial at replay time.
    if (auto rev_it = addr_to_mrp_groups_.find(addr); rev_it != addr_to_mrp_groups_.end()) {
        // Move the names out and erase addr's entry first: evict's per-member
        // sweep edits the reverse map (would invalidate rev_it) and skips addr
        // for free (replaces the old `if (other == addr) continue;` guard).
        auto group_names = std::move(rev_it->second);
        addr_to_mrp_groups_.erase(rev_it);
        for (const auto &name : group_names)
            evict_mrp_group_locked(name);
    }
    // pending_outputs_ is a subset of poly_map_; erase-on-miss is O(1), so
    // unconditional double-erase is simpler than tagging addrs by owner.
    poly_map_.erase(addr);
    pending_outputs_.erase(addr);
    addr_modulus_.erase(addr);
    input_addrs_.erase(addr);
    // A recycled allocation leading a new group gets a fresh name rather
    // than colliding with a possibly-pending group from its previous life.
    mrp_in_names_.erase(addr);
    mrp_out_names_.erase(addr);
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = config().ring_dim();
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

std::expected<void, HazeInternalError> EpochState::tag_output_locked(DevAddr addr) {
    if (!poly_map_.contains(addr)) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "tag_output_locked: addr not bound in poly_map_");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }
    // An MRP residue tags every residue of its group and promotes the group.
    if (auto rev = addr_to_mrp_groups_.find(addr); rev != addr_to_mrp_groups_.end()) {
        // Registration keeps an addr in at most one group (and erases empty
        // reverse entries), so this set holds exactly one name; the loop is
        // kept as defense in depth should that invariant ever relax.
        assert(rev->second.size() == 1);
        for (const auto &group_name : rev->second) {
            auto g = known_mrp_groups_.find(group_name);
            if (g == known_mrp_groups_.end())
                continue;
            for (DevAddr a : g->second.addrs)
                if (!pending_outputs_.contains(a))
                    pending_outputs_.emplace(a, "haze_out_" + std::to_string(output_counter_++));
            pending_mrp_groups_.insert(group_name);
        }
        return {};
    }
    if (!pending_outputs_.contains(addr))
        pending_outputs_.emplace(addr, "haze_out_" + std::to_string(output_counter_++));
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
    // Both guards below cover invariants that the H2D entry point has
    // already enforced (copy_h2d requires alloc_set_ membership and a
    // configured poly_bytes_, so by the time this runs ring_dim is set
    // and shadow bytes exist). Treat hits as broken-haze internal
    // errors so we don't silently drop the input tag.
    const uint64_t ring_dim = config().ring_dim();
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "tag_h2d_input_locked: ring_dim == 0 after copy_h2d");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
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
    if (auto [it, inserted] = mrp_input_tagged_names_.insert(name); inserted) {
        fhetch::tag_input(*it, mrp);
    }
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
    // Latest-write-wins registration. Identical re-registration (same op
    // re-run, e.g. an in-place accumulation loop) is a cheap no-op; anything
    // else replaces stale group state so a tagged readback assembles the
    // membership of the most recent write, never a stale addr list / moduli.
    auto existing = known_mrp_groups_.find(name);
    if (existing != known_mrp_groups_.end() && existing->second.addrs.size() == addrs.size() &&
        std::equal(addrs.begin(), addrs.end(), existing->second.addrs.begin()) &&
        std::equal(moduli.begin(), moduli.end(), existing->second.moduli.begin())) {
        // Safe to skip the conflict sweep below: any competing registration
        // since this group's last write would have evicted it (existing would
        // be end()), so no other group can claim these addrs right now.
        return {};
    }

    // Any other group claiming one of the new addrs is falsified by this
    // write: its multi-residue claim no longer describes what the addr holds.
    // Evict it wholesale (mirrors invalidate() on free). Members keep their
    // poly_map_ bindings and per-residue tags as standalone SRP values.
    for (DevAddr a : addrs) {
        auto rev = addr_to_mrp_groups_.find(a);
        if (rev == addr_to_mrp_groups_.end())
            continue;
        // Copy: evict_mrp_group_locked edits the reverse map under us.
        std::vector<std::string> conflicting(rev->second.begin(), rev->second.end());
        for (const auto &other : conflicting)
            if (other != name)
                evict_mrp_group_locked(other);
    }

    if (existing != known_mrp_groups_.end()) {
        // Same name, new shape (e.g. an in-place rescale re-using dst[0] with
        // fewer residues): replace membership in place. pending_mrp_groups_
        // stores names, so an already-tagged group exports the replacement.
        for (DevAddr old_addr : existing->second.addrs) {
            auto o = addr_to_mrp_groups_.find(old_addr);
            if (o == addr_to_mrp_groups_.end())
                continue;
            o->second.erase(name);
            if (o->second.empty())
                addr_to_mrp_groups_.erase(o);
        }
        existing->second.addrs.assign(addrs.begin(), addrs.end());
        existing->second.moduli.assign(moduli.begin(), moduli.end());
        for (DevAddr a : addrs)
            addr_to_mrp_groups_[a].insert(name);
        // A tagged group's members each carry a per-residue output tag so
        // flush-time shadow population covers them; extend that to members
        // introduced by the replacement.
        if (pending_mrp_groups_.contains(name)) {
            for (DevAddr a : addrs)
                if (!pending_outputs_.contains(a))
                    pending_outputs_.emplace(a, "haze_out_" + std::to_string(output_counter_++));
        }
        return {};
    }

    auto [it, inserted] = known_mrp_groups_.try_emplace(std::move(name));
    it->second.addrs.assign(addrs.begin(), addrs.end());
    it->second.moduli.assign(moduli.begin(), moduli.end());
    for (DevAddr a : addrs)
        addr_to_mrp_groups_[a].insert(it->first);
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

    // Also tag each MRP group as a fhetch MRP output so external callers
    // can pull the multi-residue view via fhetch::result(name, MRP&).
    // pending_mrp_groups_ holds names; resolve through known_mrp_groups_ so
    // the membership exported is the latest registration for that name.
    for (const auto &name : pending_mrp_groups_) {
        auto g_it = known_mrp_groups_.find(name);
        if (g_it == known_mrp_groups_.end()) {
            // pending ⊆ known is maintained by registration/eviction; a miss
            // here is a state-management bug, not recoverable.
            std::ostringstream body;
            body << "tag_pending_outputs_locked: pending MRP group '" << name
                 << "' missing from known_mrp_groups_";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        const auto &g = g_it->second;
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g.addrs.size());
        for (size_t i = 0; i < g.addrs.size(); ++i) {
            auto it = poly_map_.find(g.addrs[i]);
            if (it == poly_map_.end()) {
                // Group registered but its poly_map_ binding was invalidated before materialize.
                std::ostringstream body;
                body << "tag_pending_outputs_locked: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g.addrs[i]) << std::dec << " missing from poly_map_";
                record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
                return std::unexpected(HazeInternalError::MissingPolyMapBinding);
            }
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(it->second, g.moduli[i]);
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
    if (pending_outputs_.empty() && pending_mrp_groups_.empty()) {
        return {};
    }

    if (auto tagged = tag_pending_outputs_locked(); !tagged)
        return std::unexpected(tagged.error());

    return write_trace_and_replay_locked(run_replay);
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

std::expected<void, HazeInternalError> EpochState::write_trace_and_replay_locked(bool run_replay) {
    if (!recording_) {
        return {};
    }

    // Step 1: write the trace. The replay_bridge post-recording hook
    // runs inside the vendor stop(); we drain its failure flag right after.
    const bool stop_ok = CompilerBackend::stop_recording();
    if (!stop_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::write_trace_and_replay_locked (stop_recording)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    if (hazeReplayBridgeTakeHookHadError() != 0) {
        clear_state_locked();
        record_internal_error(
            HazeInternalError::BridgeHookFailed,
            "post_recording_hook reported per-input/output failures (see prior log entries)");
        return std::unexpected(HazeInternalError::BridgeHookFailed);
    }

    // hazeWriteProgram() stops here: step 1 has written the full program dir
    // (.fhetch + inputs + templates + cryptocontext), ready to ship for
    // out-of-process replay (e.g. on the FPGA host). There is no in-process
    // result to read back, so skip replay + shadow population.
    if (!run_replay) {
        clear_state_locked();
        return {};
    }

    // Step 2: dispatch replay. kLocalTarget runs the in-process simulator;
    // other targets spawn nbcc_fhetch_replay over HTTP — both produce
    // serialized_probes/<name>.ct for step 3 to read.
    const bool replay_ok = CompilerBackend::replay();
    if (!replay_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::write_trace_and_replay_locked (replay)");
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
        if (!decode_result_values(result_poly, values)) {
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

    clear_state_locked();
    return {};
}

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    pending_outputs_.clear();
    addr_modulus_.clear();
    input_addrs_.clear();
    known_mrp_groups_.clear();
    pending_mrp_groups_.clear();
    addr_to_mrp_groups_.clear();
    mrp_input_tagged_names_.clear();
    mrp_in_names_.clear();
    mrp_out_names_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
    mrp_in_name_counter_ = 0;
    mrp_out_name_counter_ = 0;
    // Mirror clears to libnbfhetch so a failed materialise can't leak
    // captures into the next epoch; pairs with EpochSession's setup.
    CompilerBackend::clear_captured();
}

std::string EpochState::mrp_group_name_locked(bool output, DevAddr leading) {
    auto &names = output ? mrp_out_names_ : mrp_in_names_;
    if (auto it = names.find(leading); it != names.end())
        return it->second;
    auto &counter = output ? mrp_out_name_counter_ : mrp_in_name_counter_;
    std::string name = (output ? "haze_mrp_out_" : "haze_mrp_in_") + std::to_string(counter++);
    names.emplace(leading, name);
    return name;
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

HazeMutex &EpochSession::init_then_get_mutex() noexcept {
    // Run ensure_initialized() before grabbing the epoch lock so first-call
    // init doesn't serialize; failure surfaces later via is_initialized().
    [[maybe_unused]] const bool _ = backend().ensure_initialized();
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
    // Montgomery / bit-reversed traces can't execute on the in-process
    // simulator. ensure_initialized() already refuses first-time init for this
    // combination (so nothing was recorded); checking again here makes the
    // failure visible at the flush call instead of a silent no-op followed
    // by OutputNotFlushed on the next D2H, and also covers the
    // flags-set-after-init ordering the init-time check can't see.
    if ((config().montgomery() || config().bit_reversal()) && config().target() == kLocalTarget) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "haze::flush (montgomery/bit_reversal require a transport "
                              "target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
    }
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
