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

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haze {

// Per-epoch MRP group bookkeeping (membership, pending output export, stable per-leading-addr
// names); a pure value type — no locks, no fhetch, no polymap — owned by EpochState under its
// mutex. Invariants: an addr belongs to AT MOST one group; registration is latest-write-wins
// (identical re-registration no-ops, any other overlap evicts the competing group wholesale);
// pending names are always a subset of known names.
class MrpGroupRegistry {
  public:
    struct Group {
        std::vector<DevAddr> addrs;   // residue addrs in encounter order
        std::vector<uint64_t> moduli; // base[i] paired with addrs[i]
    };

    // Drop every group that names `addr`, plus its leading-addr names, so a
    // recycled allocation cannot resurrect stale state.
    void invalidate(DevAddr addr);

    // Stable counter name ("haze_mrp_in_N" / "haze_mrp_out_N") for the group
    // led by `leading`; invalidate() drops it.
    std::string group_name(bool output, DevAddr leading);

    // True exactly once per name; the caller then emits the fhetch input tag.
    bool mark_input_tagged(const std::string &name);

    // Record `addrs`/`moduli` as the current MRP group `name` (latest-write-wins), returning
    // true if it was already a tagged output so the caller re-tags its members.
    std::expected<bool, HazeInternalError> record_mrp_group(std::span<const DevAddr> addrs,
                                                            std::span<const uint64_t> moduli,
                                                            std::string &&name);

    // Mark `addr`'s MRP group as a tagged output and return its residue addrs, or nullopt if
    // `addr` is in no group.
    std::optional<std::vector<DevAddr>> mark_group_output(DevAddr addr);

    // Pending-group view for flush-time export (resolved through the latest
    // registration).
    bool has_pending() const { return !pending_.empty(); }
    std::vector<std::string> pending_names() const;
    const Group *find(const std::string &name) const;

    void clear();

  private:
    void evict_group(const std::string &name);

    std::unordered_map<std::string, Group> known_;
    std::unordered_set<std::string> pending_; // subset of known_ keys
    // Reverse index addr -> group names (at most one after any registration).
    std::unordered_map<DevAddr, std::unordered_set<std::string>> addr_to_groups_;
    std::unordered_map<DevAddr, std::string> in_names_;
    std::unordered_map<DevAddr, std::string> out_names_;
    std::unordered_set<std::string> input_tagged_;
    uint64_t in_counter_ = 0;
    uint64_t out_counter_ = 0;
};

} // namespace haze
