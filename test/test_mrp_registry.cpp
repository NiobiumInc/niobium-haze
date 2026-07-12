// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Direct unit tests for MrpGroupRegistry — the invariants previously only
// reachable through the full C-ABI + fhetch stack (test_mrp_group_reuse.cpp).

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/mrp_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using haze::DevAddr;
using haze::MrpGroupRegistry;

DevAddr addr(uint64_t n) {
    return DevAddr{0x4000000000ULL + (n * 0x8000)};
}

std::vector<DevAddr> addrs(std::initializer_list<uint64_t> ns) {
    std::vector<DevAddr> out;
    for (auto n : ns)
        out.push_back(addr(n));
    return out;
}

const std::vector<uint64_t> kQ2 = {97, 193};
const std::vector<uint64_t> kQ3 = {97, 193, 389};

} // namespace

TEST_CASE("registry: group names are stable per leading addr and side", "[unit]") {
    MrpGroupRegistry reg;
    const auto in0 = reg.group_name(false, addr(1));
    const auto out0 = reg.group_name(true, addr(1));
    REQUIRE(in0 == "haze_mrp_in_0");
    REQUIRE(out0 == "haze_mrp_out_0");
    REQUIRE(reg.group_name(false, addr(1)) == in0); // stable on re-ask
    REQUIRE(reg.group_name(false, addr(2)) == "haze_mrp_in_1");
    // invalidate() drops the name so a recycled addr gets a fresh one.
    reg.invalidate(addr(1));
    REQUIRE(reg.group_name(false, addr(1)) == "haze_mrp_in_2");
}

TEST_CASE("registry: input tag dedup fires exactly once per name", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.mark_input_tagged("g"));
    REQUIRE_FALSE(reg.mark_input_tagged("g"));
    reg.clear();
    REQUIRE(reg.mark_input_tagged("g"));
}

TEST_CASE("registry: addr/moduli length mismatch is rejected", "[unit]") {
    MrpGroupRegistry reg;
    const auto a = addrs({1, 2});
    const std::vector<uint64_t> one = {97};
    auto r = reg.record_mrp_group(a, one, "g");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == haze::HazeInternalError::MrpGroupAddrModuliMismatch);
    REQUIRE(reg.find("g") == nullptr);
}

TEST_CASE("registry: identical re-registration is a no-op", "[unit]") {
    MrpGroupRegistry reg;
    const auto a = addrs({1, 2});
    REQUIRE(reg.record_mrp_group(a, kQ2, "g").has_value());
    auto again = reg.record_mrp_group(a, kQ2, "g");
    REQUIRE(again.has_value());
    REQUIRE_FALSE(again.value_or(true)); // no re-tag needed
    REQUIRE(reg.find("g") != nullptr);
}

TEST_CASE("registry: conflicting registration evicts the competing group", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2}), kQ2, "g1").has_value());
    // g2 claims addr 2: g1 no longer describes what addr 2 holds.
    REQUIRE(reg.record_mrp_group(addrs({2, 3}), kQ2, "g2").has_value());
    REQUIRE(reg.find("g1") == nullptr);
    REQUIRE(reg.find("g2") != nullptr);
    // addr 1 now belongs to no group.
    REQUIRE_FALSE(reg.mark_group_output(addr(1)).has_value());
}

TEST_CASE("registry: promote marks pending and expands the whole group", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2, 3}), kQ3, "g").has_value());
    REQUIRE(reg.pending_names().empty());
    auto members = reg.mark_group_output(addr(2));
    REQUIRE(members.has_value());
    REQUIRE(members.value_or(std::vector<DevAddr>{}) == addrs({1, 2, 3}));
    REQUIRE(reg.pending_names() == std::vector<std::string>{"g"});
    // Pending stays a subset of known: evicting the group clears it.
    reg.invalidate(addr(1));
    REQUIRE(reg.pending_names().empty());
    REQUIRE(reg.find("g") == nullptr);
}

TEST_CASE("registry: replacing a pending group demands re-tagging its members", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2, 3}), kQ3, "g").has_value());
    REQUIRE(reg.mark_group_output(addr(1)).has_value());
    // In-place rescale reuses the leading addr with fewer residues.
    auto replaced = reg.record_mrp_group(addrs({1, 2}), kQ2, "g");
    REQUIRE(replaced.has_value());
    REQUIRE(replaced.value_or(false)); // pending group: caller re-tags
    const auto *g = reg.find("g");
    REQUIRE(g != nullptr);
    REQUIRE(g->addrs == addrs({1, 2}));
    REQUIRE(g->moduli == kQ2);
    // The dropped member no longer maps to the group.
    REQUIRE_FALSE(reg.mark_group_output(addr(3)).has_value());
}

TEST_CASE("registry: replacement of a non-pending group needs no new tags", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2, 3}), kQ3, "g").has_value());
    auto replaced = reg.record_mrp_group(addrs({1, 2}), kQ2, "g");
    REQUIRE(replaced.has_value());
    REQUIRE_FALSE(replaced.value_or(true));
}

TEST_CASE("registry: clear resets groups, pending, names, and counters", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2}), kQ2, "g").has_value());
    REQUIRE(reg.mark_group_output(addr(1)).has_value());
    REQUIRE(reg.group_name(false, addr(1)) == "haze_mrp_in_0");
    REQUIRE(reg.group_name(true, addr(1)) == "haze_mrp_out_0");
    reg.clear();
    REQUIRE(reg.find("g") == nullptr);
    REQUIRE_FALSE(reg.has_pending());
    REQUIRE_FALSE(reg.mark_group_output(addr(1)).has_value());
    REQUIRE(reg.group_name(false, addr(1)) == "haze_mrp_in_0"); // counters restart
    REQUIRE(reg.group_name(true, addr(1)) == "haze_mrp_out_0");
}

TEST_CASE("registry: invalidate detaches only the touched addr's group", "[unit]") {
    MrpGroupRegistry reg;
    REQUIRE(reg.record_mrp_group(addrs({1, 2}), kQ2, "g1").has_value());
    REQUIRE(reg.record_mrp_group(addrs({3, 4}), kQ2, "g2").has_value());
    reg.invalidate(addr(1));
    REQUIRE(reg.find("g1") == nullptr);
    REQUIRE(reg.find("g2") != nullptr);
    REQUIRE(reg.mark_group_output(addr(3)).has_value());
}
