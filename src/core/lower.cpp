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
#include "core/backend.hpp"
#include "core/graph.hpp"
#include "core/polynomial_io.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/replay_bridge.h>
#include <ios>
#include <niobium/compiler.h>
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
    // members; consumed by the scan and dropped (only the DerivedState
    // view outlives it).
    std::unordered_map<std::string, PendingMrpGroup> known_mrp_groups;
    std::unordered_map<DevAddr, std::unordered_set<std::string>> addr_to_mrp_groups;
    std::unordered_set<std::string> mrp_input_tagged_names;
    uint64_t input_counter = 0;
    uint64_t output_counter = 0;

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
                for (const auto &group_name : rev->second) {
                    auto g = known_mrp_groups.find(group_name);
                    if (g == known_mrp_groups.end())
                        continue;
                    for (DevAddr a : g->second.addrs)
                        if (!d.pending_outputs.contains(a))
                            d.pending_outputs.emplace(a, "haze_out_" +
                                                             std::to_string(output_counter++));
                    d.pending_mrp_groups.try_emplace(group_name, g->second);
                }
            } else if (!d.pending_outputs.contains(node.addr)) {
                d.pending_outputs.emplace(node.addr,
                                          "haze_out_" + std::to_string(output_counter++));
            }
            break;

        case Node::Kind::MrpInputTag:
            // Dedup so each group name reaches fhetch exactly once.
            d.emit_mrp_input[i] = mrp_input_tagged_names.insert(node.name).second;
            break;

        case Node::Kind::MrpRegister: {
            // try_emplace dedups re-registrations for the same op (the
            // leading-addr name is stable). Addr/moduli length mismatch
            // was rejected at record time.
            auto [it, inserted] = known_mrp_groups.try_emplace(node.name);
            if (inserted) {
                it->second.addrs = node.group_addrs;
                it->second.moduli = node.group_moduli;
                for (DevAddr a : node.group_addrs)
                    addr_to_mrp_groups[a].insert(it->first);
            }
            break;
        }

        case Node::Kind::Invalidate:
            // Port of EpochState::invalidate: drop any MRP group naming
            // the addr so a recycled allocation can't bind a stale group
            // entry at materialize time, then drop the binding + tag.
            if (auto rev_it = addr_to_mrp_groups.find(node.addr);
                rev_it != addr_to_mrp_groups.end()) {
                auto group_names = std::move(rev_it->second);
                addr_to_mrp_groups.erase(rev_it);
                for (const auto &name : group_names) {
                    auto group_it = known_mrp_groups.find(name);
                    if (group_it == known_mrp_groups.end())
                        continue;
                    for (DevAddr other : group_it->second.addrs) {
                        if (other == node.addr)
                            continue;
                        auto o = addr_to_mrp_groups.find(other);
                        if (o == addr_to_mrp_groups.end())
                            continue;
                        o->second.erase(name);
                        if (o->second.empty())
                            addr_to_mrp_groups.erase(o);
                    }
                    known_mrp_groups.erase(group_it);
                    d.pending_mrp_groups.erase(name);
                }
            }
            d.final_bindings.erase(node.addr);
            d.pending_outputs.erase(node.addr);
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

std::expected<const fhetch::Polynomial *, HazeInternalError>
LowerCtx::poly(ValueId id) const noexcept {
    if (auto it = values_.find(id); it != values_.end())
        return &it->second;
    record_internal_error(HazeInternalError::MissingPolyMapBinding,
                          "LowerCtx::poly: value not materialized");
    return std::unexpected(HazeInternalError::MissingPolyMapBinding);
}

void LowerCtx::bind(ValueId id, fhetch::Polynomial poly) {
    values_.insert_or_assign(id, std::move(poly));
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
    for (const auto &[name, g] : d.pending_mrp_groups)
        for (DevAddr a : g.addrs)
            if (auto it = d.final_bindings.find(a); it != d.final_bindings.end())
                keep.insert(it->second);
    return keep;
}

std::expected<void, HazeInternalError> fail(HazeInternalError err, const char *context) {
    niobium::compiler().clear_captured();
    record_internal_error(err, context);
    return std::unexpected(err);
}

} // namespace

std::expected<void, HazeInternalError> finalize(bool run_replay) noexcept {
    HazeLockGuard lock(g_lower_mutex);

    // Flush with nothing ever recorded keeps today's silent no-op —
    // including NOT initializing the backend, so a bare hazeFlush still
    // has no program-dir side effects.
    if (graph().size() == 0)
        return {};

    // Failed-init parity with the eager engine's recording_=false
    // flush: report success, leave the tape intact for a later retry.
    if (!backend().ensure_initialized())
        return {};

    const std::vector<Node> tape = graph().seal();
    DerivedState d = derive(tape);

    if (!d.has_outputs()) {
        // Recording happened (e.g. H2D eager-tag) but nothing was
        // declared an output: skip the write/replay entirely so the
        // bridge crypto context isn't a prerequisite for compute-free
        // D2H reads. Mirrors clear_state_locked's captured-state clear.
        niobium::compiler().clear_captured();
        return {};
    }

    const std::unordered_set<ValueId> keep = values_needed_at_end(d);

    // start_epoch() before start() memorizes the polynomial-ID base so
    // post-materialize resets snap back to it; both moved here from
    // first-compute time — every fhetch emission still happens after
    // them, in tape order, exactly as the eager engine interleaved.
    niobium::compiler().start_epoch();
    CompilerBackend::start_recording();

    LowerCtx ctx;
    ctx.derived_ = &d;
    for (size_t i = 0; i < tape.size(); ++i) {
        const Node &node = tape[i];
        ctx.node_idx_ = i;
        if (node.thunk) {
            if (auto lowered = node.thunk(ctx); !lowered)
                return fail(lowered.error(), node.entry);
        }
        // Drop values whose final consumer just ran (keeps the lowering
        // pass's live set at the eager engine's level: roughly one
        // polynomial per bound address).
        for (ValueId v : node.src_vids)
            if (auto lu = d.last_use.find(v);
                lu != d.last_use.end() && lu->second == i && !keep.contains(v))
                ctx.values_.erase(v);
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
            return fail(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
        }
        const auto poly = ctx.poly(bound->second);
        if (!poly)
            return fail(poly.error(), "finalize: pending output value missing");
        fhetch::tag_output(name, **poly);
    }

    // Also tag each MRP group as a fhetch MRP output so external
    // callers can pull the multi-residue view via result(name, MRP&).
    for (const auto &[name, g] : d.pending_mrp_groups) {
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g.addrs.size());
        for (size_t i = 0; i < g.addrs.size(); ++i) {
            const auto bound = d.final_bindings.find(g.addrs[i]);
            if (bound == d.final_bindings.end()) {
                std::ostringstream body;
                body << "finalize: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g.addrs[i]) << std::dec << " has no final binding";
                return fail(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            }
            const auto poly = ctx.poly(bound->second);
            if (!poly)
                return fail(poly.error(), "finalize: MRP group value missing");
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(**poly, g.moduli[i]);
        }
        fhetch::tag_output(name, fhetch::MRP::from_pairs(pairs));
    }

    // Step 1: write the trace. The replay_bridge post-recording hook
    // runs inside stop_epoch; drain its failure flag right after.
    if (!CompilerBackend::stop_epoch())
        return fail(HazeInternalError::BackendReplayFailed, "haze::finalize (stop_epoch)");
    if (hazeReplayBridgeTakeHookHadError() != 0)
        return fail(HazeInternalError::BridgeHookFailed,
                    "post_recording_hook reported per-input/output failures (see prior log "
                    "entries)");

    // hazeWriteProgram() stops here: the full project dir is written,
    // ready to ship for out-of-process replay. No in-process result to
    // read back, so skip replay + shadow population.
    if (!run_replay) {
        niobium::compiler().clear_captured();
        return {};
    }

    // Step 2: dispatch replay (kLocalTarget = in-process simulator;
    // anything else = nbcc_fhetch_replay over HTTP).
    if (!CompilerBackend::replay())
        return fail(HazeInternalError::BackendReplayFailed, "haze::finalize (replay)");

    // Step 3: per-output shadow population. Any failure aborts the
    // epoch so a stale shadow can't surface as a silent wrong-value D2H.
    for (const auto &[addr, name] : d.pending_outputs) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::ostringstream body;
            body << "result('" << name << "') unavailable for addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            return fail(HazeInternalError::BackendOutputMissing, body.str().c_str());
        }
        std::vector<uint64_t> values;
        if (!extract_polynomial_values(result_poly, name, values)) {
            std::ostringstream body;
            body << "failed to extract values for output '" << name << "' at addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            return fail(HazeInternalError::BackendOutputDecodeFailed, body.str().c_str());
        }
        if (auto updated = allocator().update_shadow(addr, std::move(values)); !updated) {
            niobium::compiler().clear_captured();
            return std::unexpected(updated.error());
        }
    }

    niobium::compiler().clear_captured();
    return {};
}

} // namespace haze
