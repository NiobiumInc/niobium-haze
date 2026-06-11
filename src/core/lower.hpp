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
#include "core/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

// Flush-time engine for the deferred tape: derive() replays the
// record-time bookkeeping the old EpochState did eagerly, single-
// threaded over the sealed tape; finalize() then lowers the thunks
// through the fhetch API and runs the unchanged stop/replay/populate
// pipeline.

struct PendingMrpGroup {
    std::vector<DevAddr> addrs;   // residue addrs in encounter order
    std::vector<uint64_t> moduli; // base[i] paired with addrs[i]
};

// State derived from one tape scan. BYTE-IDENTITY CONSTRAINT: the
// emission order of fhetch::tag_output (and the result-population
// order) is the iteration order of these containers, which for
// unordered_map depends on the key hashing AND the emplace/erase
// sequence. They therefore use the exact container types EpochState
// used, mutated in the same order the eager engine mutated them — do
// not "clean this up" to std::map/vector without re-baselining traces.
struct DerivedState {
    std::unordered_map<DevAddr, std::string> pending_outputs;
    // Pending groups hold NAMES; membership resolves through
    // known_mrp_groups at tag time so the exported group is the latest
    // registration (latest-write-wins, mirroring the eager engine).
    std::unordered_set<std::string> pending_mrp_groups;
    std::unordered_map<std::string, PendingMrpGroup> known_mrp_groups;
    // Final addr -> value view at flush; resolves pending outputs and
    // group residues to the polynomial that lowering binds.
    std::unordered_map<DevAddr, ValueId> final_bindings;
    // Per-node derived data, indexed by tape position.
    std::vector<std::string> node_names; // haze_in_N for input nodes
    std::vector<bool> emit_mrp_input;    // MrpInputTag dedup verdicts
    // Last tape index reading each value; lets the lowering pass drop a
    // value's Polynomial as soon as its final consumer ran (mirrors the
    // eager engine's keep-only-latest-per-addr memory profile).
    std::unordered_map<ValueId, size_t> last_use;

    bool has_outputs() const noexcept {
        return !pending_outputs.empty() || !pending_mrp_groups.empty();
    }
};

// Pure, single-threaded, no fhetch calls. Faithfully replays, in tape
// order, what EpochState did at record time: input naming
// (haze_in_N), tag-output naming + MRP-group expansion (haze_out_N),
// MRP-input dedup, group registration, the invalidate group-drop walk,
// and the H2D rebind + pending-tag erase.
DerivedState derive(std::span<const Node> tape);

// Lowering context handed to each thunk. Single-threaded by
// construction (one finalize at a time; no locks, no TSA).
struct LowerCtx {
  public:
    // Value lookup for thunk operands. A miss is a haze state-keeping
    // bug (record-time validation guarantees operands were bound) and
    // maps to MissingPolyMapBinding like the eager engine's equivalent.
    std::expected<const niobium::fhetch::Polynomial *, HazeInternalError>
    poly(ValueId id) const noexcept;

    void bind(ValueId id, niobium::fhetch::Polynomial poly);

    // The derived haze_in_N name for the node currently lowering.
    const std::string &node_name() const noexcept;

    // The derived dedup verdict for the MrpInputTag node currently
    // lowering: true exactly when the eager engine would have emitted
    // the fhetch::tag_input(name, MRP).
    bool emit_mrp_input() const noexcept;

  private:
    friend std::expected<void, HazeInternalError> finalize(bool run_replay) noexcept;

    std::unordered_map<ValueId, niobium::fhetch::Polynomial> values_;
    const DerivedState *derived_ = nullptr;
    size_t node_idx_ = 0;
};

// Seal the tape, derive, lower, and run the stop/replay/populate
// pipeline. run_replay=false backs hazeWriteProgram (stop after the
// project dir is written); true backs hazeFlush. Serialized internally;
// returns without side effects when nothing was recorded.
std::expected<void, HazeInternalError> finalize(bool run_replay) noexcept;

} // namespace haze
