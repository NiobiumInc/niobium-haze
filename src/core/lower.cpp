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
#include "core/lower.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/thread_safety.hpp"
#include "core/allocator.hpp"
#include "core/graph.hpp"
#include "core/kernel_cache.hpp"
#include "core/lowering_session.hpp"
#include "core/polynomial_io.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ios>
#include <niobium/fhetch_api.h>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

DerivedState derive(std::span<const Node> tape) {
    DerivedState d;
    d.node_names.resize(tape.size());
    d.emit_mrp_input.assign(tape.size(), false);

    // Epoch-local bookkeeping the eager engine kept as EpochState
    // members; consumed by the scan (known groups + pending names
    // survive into DerivedState for the tag/populate loops).
    std::unordered_map<DevAddr, std::unordered_set<std::string>> addr_to_mrp_groups;
    std::unordered_set<std::string> mrp_input_tagged_names;
    std::unordered_map<DevAddr, std::string> mrp_in_names;
    std::unordered_map<DevAddr, std::string> mrp_out_names;
    uint64_t mrp_in_name_counter = 0;
    uint64_t mrp_out_name_counter = 0;
    uint64_t input_counter = 0;
    uint64_t output_counter = 0;

    // Stable counter name for the group led by `leading` ("haze_mrp_in_N"
    // / "haze_mrp_out_N"): same leading addr -> same name within an
    // epoch; Invalidate drops it so a recycled allocation gets a fresh
    // name. Mirror of the eager engine's mrp_group_name_locked.
    const auto group_name = [&](bool output, DevAddr leading) {
        auto &names = output ? mrp_out_names : mrp_in_names;
        if (auto it = names.find(leading); it != names.end())
            return it->second;
        auto &counter = output ? mrp_out_name_counter : mrp_in_name_counter;
        std::string name = (output ? "haze_mrp_out_" : "haze_mrp_in_") + std::to_string(counter++);
        names.emplace(leading, name);
        return name;
    };

    // Drop one group wholesale (eager engine's evict_mrp_group_locked):
    // scrub every member's reverse-map entry, erase from known + pending.
    const auto evict_group = [&](const std::string &name) {
        auto group_it = d.known_mrp_groups.find(name);
        if (group_it == d.known_mrp_groups.end())
            return;
        for (DevAddr member : group_it->second.addrs) {
            auto o = addr_to_mrp_groups.find(member);
            if (o == addr_to_mrp_groups.end())
                continue;
            o->second.erase(name);
            if (o->second.empty())
                addr_to_mrp_groups.erase(o);
        }
        d.known_mrp_groups.erase(group_it);
        d.pending_mrp_groups.erase(name);
    };

    for (size_t i = 0; i < tape.size(); ++i) {
        const Node &node = tape[i];
        switch (node.kind) {
        case Node::Kind::InputSnapshot:
        case Node::Kind::H2DInput:
            d.node_names[i] = "haze_in_" + std::to_string(input_counter++);
            d.final_bindings.insert_or_assign(node.addr, node.dst_vid);
            if (node.kind == Node::Kind::H2DInput) {
                // New H2D bytes are the truth at addr: a re-uploaded
                // input is not an output. The consumed haze_out_N is
                // not refunded; MRP-group claims stay.
                d.pending_outputs.erase(node.addr);
            }
            break;

        case Node::Kind::Compute:
        case Node::Kind::Copy:
            if (!node.group_addrs.empty()) {
                for (size_t j = 0; j < node.group_addrs.size(); ++j)
                    d.final_bindings.insert_or_assign(node.group_addrs[j], node.group_vids[j]);
            } else {
                d.final_bindings.insert_or_assign(node.addr, node.dst_vid);
            }
            break;

        case Node::Kind::TagOutput:
            // An MRP residue tags every residue of its group and
            // promotes the group (port of tag_output_locked).
            if (auto rev = addr_to_mrp_groups.find(node.addr); rev != addr_to_mrp_groups.end()) {
                for (const auto &name : rev->second) {
                    auto g = d.known_mrp_groups.find(name);
                    if (g == d.known_mrp_groups.end())
                        continue;
                    for (DevAddr a : g->second.addrs)
                        if (!d.pending_outputs.contains(a))
                            d.pending_outputs.emplace(a, "haze_out_" +
                                                             std::to_string(output_counter++));
                    d.pending_mrp_groups.insert(name);
                }
            } else if (!d.pending_outputs.contains(node.addr)) {
                d.pending_outputs.emplace(node.addr,
                                          "haze_out_" + std::to_string(output_counter++));
            }
            break;

        case Node::Kind::MrpInputTag:
            // Counter-based name from the leading addr; dedup so each
            // name reaches fhetch exactly once (the thunk reads it back
            // via ctx.node_name(); memoized clones carry a TRANSLATED
            // leading addr, so instances name like cold recordings).
            d.node_names[i] = group_name(/*output=*/false, node.addr);
            d.emit_mrp_input[i] = mrp_input_tagged_names.insert(d.node_names[i]).second;
            break;

        case Node::Kind::MrpRegister: {
            // Latest-write-wins registration (port of the eager
            // register_mrp_output_group_locked): identical re-registration
            // is a no-op; anything else evicts conflicting claims and
            // replaces stale membership so a tagged readback assembles
            // the most recent write.
            const std::string name = group_name(/*output=*/true, node.addr);
            auto existing = d.known_mrp_groups.find(name);
            if (existing != d.known_mrp_groups.end() &&
                existing->second.addrs == node.group_addrs &&
                existing->second.moduli == node.group_moduli)
                break; // identical: competing claims were already evicted

            for (DevAddr a : node.group_addrs) {
                auto rev = addr_to_mrp_groups.find(a);
                if (rev == addr_to_mrp_groups.end())
                    continue;
                // Copy: evict_group edits the reverse map under us.
                const std::vector<std::string> conflicting(rev->second.begin(), rev->second.end());
                for (const auto &other : conflicting)
                    if (other != name)
                        evict_group(other);
            }

            if (existing != d.known_mrp_groups.end()) {
                // Same name, new shape: replace membership in place; an
                // already-tagged group exports the replacement, and newly
                // introduced members get their per-residue output tags.
                for (DevAddr old_addr : existing->second.addrs) {
                    auto o = addr_to_mrp_groups.find(old_addr);
                    if (o == addr_to_mrp_groups.end())
                        continue;
                    o->second.erase(name);
                    if (o->second.empty())
                        addr_to_mrp_groups.erase(o);
                }
                existing->second.addrs = node.group_addrs;
                existing->second.moduli = node.group_moduli;
                for (DevAddr a : node.group_addrs)
                    addr_to_mrp_groups[a].insert(name);
                if (d.pending_mrp_groups.contains(name)) {
                    for (DevAddr a : node.group_addrs)
                        if (!d.pending_outputs.contains(a))
                            d.pending_outputs.emplace(a, "haze_out_" +
                                                             std::to_string(output_counter++));
                }
                break;
            }
            auto [it, inserted] = d.known_mrp_groups.try_emplace(name);
            it->second.addrs = node.group_addrs;
            it->second.moduli = node.group_moduli;
            for (DevAddr a : node.group_addrs)
                addr_to_mrp_groups[a].insert(it->first);
            break;
        }

        case Node::Kind::Invalidate:
            // Port of EpochState::invalidate: drop any MRP group naming
            // the addr, the binding, the tag, and the leading-addr name
            // claims (a recycled allocation gets a fresh counter name).
            if (auto rev_it = addr_to_mrp_groups.find(node.addr);
                rev_it != addr_to_mrp_groups.end()) {
                auto group_names = std::move(rev_it->second);
                addr_to_mrp_groups.erase(rev_it);
                for (const auto &name : group_names)
                    evict_group(name);
            }
            d.final_bindings.erase(node.addr);
            d.pending_outputs.erase(node.addr);
            mrp_in_names.erase(node.addr);
            mrp_out_names.erase(node.addr);
            break;
        }

        for (ValueId v : node.src_vids)
            d.last_use[v] = i;
        if (node.kind == Node::Kind::MrpInputTag)
            for (ValueId v : node.group_vids)
                d.last_use[v] = i;
    }

    return d;
}

ValueId LowerCtx::translate(ValueId id) const noexcept {
    if (remap_ == nullptr)
        return id;
    if (const auto it = remap_->find(id); it != remap_->end())
        return it->second;
    return id;
}

std::expected<const fhetch::Polynomial *, HazeInternalError>
LowerCtx::poly(ValueId id) const noexcept {
    if (auto it = values_.find(translate(id)); it != values_.end())
        return &it->second;
    record_internal_error(HazeInternalError::MissingPolyMapBinding,
                          "LowerCtx::poly: value not materialized");
    return std::unexpected(HazeInternalError::MissingPolyMapBinding);
}

void LowerCtx::bind(ValueId id, fhetch::Polynomial poly) {
    values_.insert_or_assign(translate(id), std::move(poly));
}

const std::string &LowerCtx::node_name() const noexcept {
    return derived_->node_names[node_idx_];
}

bool LowerCtx::emit_mrp_input() const noexcept {
    return derived_->emit_mrp_input[node_idx_];
}

namespace {

// Serializes finalize() against itself; the record path never takes it.
HazeMutex g_lower_mutex;

// Values that must stay materialized through output tagging and shadow
// population: the final binding of every pending output addr and every
// pending MRP-group residue.
std::unordered_set<ValueId> values_needed_at_end(const DerivedState &d) {
    std::unordered_set<ValueId> keep;
    for (const auto &[addr, name] : d.pending_outputs)
        if (auto it = d.final_bindings.find(addr); it != d.final_bindings.end())
            keep.insert(it->second);
    for (const auto &name : d.pending_mrp_groups) {
        const auto g = d.known_mrp_groups.find(name);
        if (g == d.known_mrp_groups.end())
            continue;
        for (DevAddr a : g->second.addrs)
            if (auto it = d.final_bindings.find(a); it != d.final_bindings.end())
                keep.insert(it->second);
    }
    return keep;
}

std::expected<void, HazeInternalError> fail(LoweringSession &session, HazeInternalError err,
                                            const char *context) {
    session.clear_captured();
    record_internal_error(err, context);
    return std::unexpected(err);
}

} // namespace

std::expected<void, HazeInternalError> finalize(bool run_replay) noexcept {
    HazeLockGuard lock(g_lower_mutex);

    // Flushing inside an open hazeKernelBegin bracket would seal (and
    // discard the bindings of) values the bracket still references —
    // refuse loudly instead of memoizing a sub-tape with dangling vids.
    if (kernel_cache().has_open_frame()) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeFlush/hazeWriteProgram inside an open kernel bracket");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }

    // Flush with nothing ever recorded keeps today's silent no-op —
    // including NOT initializing the backend, so a bare hazeFlush still
    // has no program-dir side effects.
    if (graph().size() == 0)
        return {};

    // One control handle per flush — the seam the planned fhetch
    // context API plugs into (see lowering_session.hpp).
    LoweringSession session;

    // Failed-init parity with the eager engine's recording_=false
    // flush: report success, leave the tape intact for a later retry.
    if (!session.ensure_backend())
        return {};

    const std::vector<Node> tape = graph().seal();
    bindings().clear();
    recorded_moduli().clear();
    DerivedState d = derive(tape);

    if (!d.has_outputs()) {
        // Recording happened (e.g. H2D eager-tag) but nothing was
        // declared an output: skip the write/replay entirely so the
        // bridge crypto context isn't a prerequisite for compute-free
        // D2H reads. Mirrors clear_state_locked's captured-state clear.
        session.clear_captured();
        return {};
    }

    const std::unordered_set<ValueId> keep = values_needed_at_end(d);

    // start_epoch() before start() memorizes the polynomial-ID base so
    // post-materialize resets snap back to it; both moved here from
    // first-compute time — every fhetch emission still happens after
    // them, in tape order, exactly as the eager engine interleaved.
    session.begin_epoch();

    LowerCtx ctx;
    ctx.derived_ = &d;
    ctx.session_ = &session;
    for (size_t i = 0; i < tape.size(); ++i) {
        const Node &node = tape[i];
        ctx.node_idx_ = i;
        ctx.remap_ = node.vid_remap.get();
        if (node.thunk) {
            if (auto lowered = node.thunk(ctx); !lowered)
                return fail(session, lowered.error(), node.entry);
        }
        // Drop values whose final consumer just ran (keeps the lowering
        // pass's live set at the eager engine's level: roughly one
        // polynomial per bound address). MrpInputTag nodes read their
        // residues through group_vids, mirroring derive()'s last_use.
        const auto evict_if_done = [&](ValueId v) {
            if (auto lu = d.last_use.find(v);
                lu != d.last_use.end() && lu->second == i && !keep.contains(v))
                ctx.values_.erase(v);
        };
        for (ValueId v : node.src_vids)
            evict_if_done(v);
        if (node.kind == Node::Kind::MrpInputTag)
            for (ValueId v : node.group_vids)
                evict_if_done(v);
    }

    // Output tagging (port of tag_pending_outputs_locked). A pending
    // output without a final binding is a state-keeping bug, not
    // recoverable.
    for (const auto &[addr, name] : d.pending_outputs) {
        const auto bound = d.final_bindings.find(addr);
        if (bound == d.final_bindings.end()) {
            std::ostringstream body;
            body << "finalize: pending output '" << name << "' addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec << " has no final binding";
            return fail(session, HazeInternalError::MissingPolyMapBinding, body.str().c_str());
        }
        const auto poly = ctx.poly(bound->second);
        if (!poly)
            return fail(session, poly.error(), "finalize: pending output value missing");
        fhetch::tag_output(name, **poly);
    }

    // Also tag each MRP group as a fhetch MRP output so external
    // callers can pull the multi-residue view via result(name, MRP&).
    // Pending holds names; resolve through known_mrp_groups so the
    // membership exported is the latest registration for that name.
    for (const auto &name : d.pending_mrp_groups) {
        const auto g_it = d.known_mrp_groups.find(name);
        if (g_it == d.known_mrp_groups.end()) {
            std::ostringstream body;
            body << "finalize: pending MRP group '" << name << "' missing from known groups";
            return fail(session, HazeInternalError::MissingPolyMapBinding, body.str().c_str());
        }
        const PendingMrpGroup &g = g_it->second;
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g.addrs.size());
        for (size_t i = 0; i < g.addrs.size(); ++i) {
            const auto bound = d.final_bindings.find(g.addrs[i]);
            if (bound == d.final_bindings.end()) {
                std::ostringstream body;
                body << "finalize: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g.addrs[i]) << std::dec << " has no final binding";
                return fail(session, HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            }
            const auto poly = ctx.poly(bound->second);
            if (!poly)
                return fail(session, poly.error(), "finalize: MRP group value missing");
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(**poly, g.moduli[i]);
        }
        fhetch::tag_output(name, fhetch::MRP::from_pairs(pairs));
    }

    // Step 1: write the trace. The replay_bridge post-recording hook
    // runs inside the trace write; drain its failure flag right after.
    if (!session.write_trace())
        return fail(session, HazeInternalError::BackendReplayFailed, "haze::finalize (stop_epoch)");
    if (session.bridge_hook_failed())
        return fail(session, HazeInternalError::BridgeHookFailed,
                    "post_recording_hook reported per-input/output failures (see prior log "
                    "entries)");

    // hazeWriteProgram() stops here: the full project dir is written,
    // ready to ship for out-of-process replay. No in-process result to
    // read back, so skip replay + shadow population.
    if (!run_replay) {
        session.clear_captured();
        return {};
    }

    // Step 2: dispatch replay (kLocalTarget = in-process simulator;
    // anything else = nbcc_fhetch_replay over HTTP).
    if (!session.replay())
        return fail(session, HazeInternalError::BackendReplayFailed, "haze::finalize (replay)");

    // Step 3: per-output shadow population. Any failure aborts the
    // epoch so a stale shadow can't surface as a silent wrong-value D2H.
    for (const auto &[addr, name] : d.pending_outputs) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::ostringstream body;
            body << "result('" << name << "') unavailable for addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            return fail(session, HazeInternalError::BackendOutputMissing, body.str().c_str());
        }
        std::vector<uint64_t> values;
        if (!extract_polynomial_values(result_poly, name, values)) {
            std::ostringstream body;
            body << "failed to extract values for output '" << name << "' at addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            return fail(session, HazeInternalError::BackendOutputDecodeFailed, body.str().c_str());
        }
        if (auto updated = allocator().update_shadow(addr, std::move(values)); !updated) {
            session.clear_captured();
            return std::unexpected(updated.error());
        }
    }

    session.clear_captured();
    return {};
}

} // namespace haze
