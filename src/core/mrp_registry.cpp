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
#include "core/mrp_registry.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace haze {

void MrpGroupRegistry::evict_group(const std::string &name) {
    auto group_it = known_.find(name);
    if (group_it == known_.end())
        return;
    for (DevAddr member : group_it->second.addrs) {
        auto o = addr_to_groups_.find(member);
        if (o == addr_to_groups_.end())
            continue;
        o->second.erase(name);
        if (o->second.empty())
            addr_to_groups_.erase(o);
    }
    known_.erase(group_it);
    pending_.erase(name);
}

void MrpGroupRegistry::invalidate(DevAddr addr) {
    if (auto rev_it = addr_to_groups_.find(addr); rev_it != addr_to_groups_.end()) {
        // Detach addr's entry first: evict_group edits the reverse map.
        auto group_names = std::move(rev_it->second);
        addr_to_groups_.erase(rev_it);
        for (const auto &name : group_names)
            evict_group(name);
    }
    // A recycled allocation leading a new group gets a fresh name.
    in_names_.erase(addr);
    out_names_.erase(addr);
}

std::string MrpGroupRegistry::group_name(bool output, DevAddr leading) {
    auto &names = output ? out_names_ : in_names_;
    if (auto it = names.find(leading); it != names.end())
        return it->second;
    auto &counter = output ? out_counter_ : in_counter_;
    std::string name = (output ? "haze_mrp_out_" : "haze_mrp_in_") + std::to_string(counter++);
    names.emplace(leading, name);
    return name;
}

bool MrpGroupRegistry::mark_input_tagged(const std::string &name) {
    return input_tagged_.insert(name).second;
}

std::expected<bool, HazeInternalError>
MrpGroupRegistry::record_mrp_group(std::span<const DevAddr> addrs, std::span<const uint64_t> moduli,
                                   std::string &&name) {
    if (addrs.size() != moduli.size()) {
        std::ostringstream body;
        body << "record_mrp_group('" << name << "'): addrs.size()=" << addrs.size()
             << " != moduli.size()=" << moduli.size();
        record_internal_error(HazeInternalError::MrpGroupAddrModuliMismatch, body.str().c_str());
        return std::unexpected(HazeInternalError::MrpGroupAddrModuliMismatch);
    }
    // Identical re-registration (e.g. an in-place accumulation loop) is a
    // no-op; a competing registration would already have evicted this group.
    auto existing = known_.find(name);
    if (existing != known_.end() && existing->second.addrs.size() == addrs.size() &&
        std::equal(addrs.begin(), addrs.end(), existing->second.addrs.begin()) &&
        std::equal(moduli.begin(), moduli.end(), existing->second.moduli.begin())) {
        return false;
    }

    // Latest write wins: any other group claiming one of the new addrs no
    // longer describes what that addr holds — evict it wholesale.
    for (DevAddr a : addrs) {
        auto rev = addr_to_groups_.find(a);
        if (rev == addr_to_groups_.end())
            continue;
        std::vector<std::string> conflicting(rev->second.begin(), rev->second.end());
        for (const auto &other : conflicting)
            if (other != name)
                evict_group(other);
    }

    if (existing != known_.end()) {
        // Same name, new shape: replace membership in place.
        for (DevAddr old_addr : existing->second.addrs) {
            auto o = addr_to_groups_.find(old_addr);
            if (o == addr_to_groups_.end())
                continue;
            o->second.erase(name);
            if (o->second.empty())
                addr_to_groups_.erase(o);
        }
        existing->second.addrs.assign(addrs.begin(), addrs.end());
        existing->second.moduli.assign(moduli.begin(), moduli.end());
        for (DevAddr a : addrs)
            addr_to_groups_[a].insert(name);
        return pending_.contains(name);
    }

    auto [it, inserted] = known_.try_emplace(std::move(name));
    it->second.addrs.assign(addrs.begin(), addrs.end());
    it->second.moduli.assign(moduli.begin(), moduli.end());
    for (DevAddr a : addrs)
        addr_to_groups_[a].insert(it->first);
    return false;
}

std::optional<std::vector<DevAddr>> MrpGroupRegistry::mark_group_output(DevAddr addr) {
    auto rev = addr_to_groups_.find(addr);
    if (rev == addr_to_groups_.end())
        return std::nullopt;
    std::vector<DevAddr> members;
    // At most one group per addr after any registration; the loop is defense
    // in depth should that invariant ever relax.
    for (const auto &gname : rev->second) {
        auto g = known_.find(gname);
        if (g == known_.end())
            continue;
        members.insert(members.end(), g->second.addrs.begin(), g->second.addrs.end());
        pending_.insert(gname);
    }
    // No live group (every mapped name diverged/evicted) — nullopt so the
    // caller falls through to SRP tagging rather than skipping `addr`.
    if (members.empty())
        return std::nullopt;
    return members;
}

std::vector<std::string> MrpGroupRegistry::pending_names() const {
    return {pending_.begin(), pending_.end()};
}

const MrpGroupRegistry::Group *MrpGroupRegistry::find(const std::string &name) const {
    auto it = known_.find(name);
    return it == known_.end() ? nullptr : &it->second;
}

void MrpGroupRegistry::clear() {
    known_.clear();
    pending_.clear();
    addr_to_groups_.clear();
    in_names_.clear();
    out_names_.clear();
    input_tagged_.clear();
    in_counter_ = 0;
    out_counter_ = 0;
}

} // namespace haze
