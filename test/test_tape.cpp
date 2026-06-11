// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Unit tests for the deferred-recording tape primitives: the global
// ValueId counter, the lock-free BindingTable (addr -> ValueId), and the
// Graph append/seal/reset lifecycle. These are internal-API tests
// (haze_tests links the haze object files directly), tagged [unit] so
// they run in the `make test-unit` sweep.

#include "common/handle.hpp"
#include "core/config.hpp"
#include "core/graph.hpp"
#include "core/lower.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kSlotBytes = 4096 * sizeof(uint64_t);

haze::DevAddr nth_slot_addr(size_t n) {
    return haze::DevAddr{haze::kHbmBase + (n * kSlotBytes)};
}

// The table and graph are process singletons; every test starts and
// ends from a clean slate so ordering between cases never matters.
struct TapeFixture {
    TapeFixture() {
        haze::bindings().set_slot_bytes(kSlotBytes);
        haze::graph().reset();
    }
    ~TapeFixture() {
        haze::graph().reset();
        haze::bindings().set_slot_bytes(0);
    }
    TapeFixture(const TapeFixture &) = delete;
    TapeFixture &operator=(const TapeFixture &) = delete;
};

} // namespace

TEST_CASE("new_value_id hands out distinct non-sentinel ids", "[unit][tape]") {
    const haze::ValueId a = haze::new_value_id();
    const haze::ValueId b = haze::new_value_id();
    REQUIRE(a != haze::kUnbound);
    REQUIRE(b != haze::kUnbound);
    REQUIRE(a != b);
}

TEST_CASE("BindingTable: load/store/erase roundtrip", "[unit][tape]") {
    TapeFixture fixture;
    const haze::DevAddr addr = nth_slot_addr(0);

    REQUIRE(haze::bindings().load(addr) == haze::kUnbound);

    const haze::ValueId id = haze::new_value_id();
    haze::bindings().store(addr, id);
    REQUIRE(haze::bindings().load(addr) == id);

    haze::bindings().erase(addr);
    REQUIRE(haze::bindings().load(addr) == haze::kUnbound);
}

TEST_CASE("BindingTable: promote binds first touch and returns the resident id", "[unit][tape]") {
    TapeFixture fixture;
    const haze::DevAddr addr = nth_slot_addr(1);

    const haze::ValueId first = haze::new_value_id();
    REQUIRE(haze::bindings().promote(addr, first) == first);

    // A second promotion must NOT rebind: the resident id wins and the
    // caller adopts it (the record path drops its fresh id + snapshot).
    const haze::ValueId second = haze::new_value_id();
    REQUIRE(haze::bindings().promote(addr, second) == first);
    REQUIRE(haze::bindings().load(addr) == first);
}

TEST_CASE("BindingTable: store overwrites an existing binding", "[unit][tape]") {
    TapeFixture fixture;
    const haze::DevAddr addr = nth_slot_addr(2);

    const haze::ValueId first = haze::new_value_id();
    const haze::ValueId second = haze::new_value_id();
    haze::bindings().store(addr, first);
    haze::bindings().store(addr, second); // result rebind / H2D re-upload
    REQUIRE(haze::bindings().load(addr) == second);
}

TEST_CASE("BindingTable: distinct slots do not alias across chunk boundaries", "[unit][tape]") {
    TapeFixture fixture;
    // Slots 4095 and 4096 land in different chunks (kChunkSlots = 4096).
    const haze::DevAddr last_of_first = nth_slot_addr(4095);
    const haze::DevAddr first_of_second = nth_slot_addr(4096);

    const haze::ValueId a = haze::new_value_id();
    const haze::ValueId b = haze::new_value_id();
    haze::bindings().store(last_of_first, a);
    haze::bindings().store(first_of_second, b);
    REQUIRE(haze::bindings().load(last_of_first) == a);
    REQUIRE(haze::bindings().load(first_of_second) == b);
}

TEST_CASE("BindingTable: unset geometry and out-of-range addrs read unbound", "[unit][tape]") {
    TapeFixture fixture;

    // Below the HBM base (e.g. a host pointer cast by mistake).
    const haze::DevAddr below{haze::kHbmBase - kSlotBytes};
    REQUIRE(haze::bindings().load(below) == haze::kUnbound);
    haze::bindings().store(below, haze::new_value_id()); // must not crash
    REQUIRE(haze::bindings().load(below) == haze::kUnbound);

    // Geometry unset: every lookup is unbound, writes are dropped.
    haze::bindings().set_slot_bytes(0);
    const haze::DevAddr addr = nth_slot_addr(0);
    haze::bindings().store(addr, haze::new_value_id());
    REQUIRE(haze::bindings().load(addr) == haze::kUnbound);
}

TEST_CASE("BindingTable: clear drops every binding", "[unit][tape]") {
    TapeFixture fixture;
    const haze::DevAddr a = nth_slot_addr(0);
    const haze::DevAddr b = nth_slot_addr(5000); // second chunk
    haze::bindings().store(a, haze::new_value_id());
    haze::bindings().store(b, haze::new_value_id());

    haze::bindings().clear();
    REQUIRE(haze::bindings().load(a) == haze::kUnbound);
    REQUIRE(haze::bindings().load(b) == haze::kUnbound);
}

TEST_CASE("Graph: seal returns nodes in append order and empties the tape", "[unit][tape]") {
    TapeFixture fixture;

    haze::Node first{};
    first.kind = haze::Node::Kind::InputSnapshot;
    first.addr = nth_slot_addr(0);
    first.entry = "test-first";
    haze::Node second{};
    second.kind = haze::Node::Kind::TagOutput;
    second.addr = nth_slot_addr(1);
    second.entry = "test-second";

    haze::graph().append(std::move(first));
    haze::graph().append(std::move(second));
    REQUIRE(haze::graph().size() == 2);

    const std::vector<haze::Node> tape = haze::graph().seal();
    REQUIRE(tape.size() == 2);
    REQUIRE(tape[0].kind == haze::Node::Kind::InputSnapshot);
    REQUIRE(tape[1].kind == haze::Node::Kind::TagOutput);
    REQUIRE(haze::graph().size() == 0);

    // seal() also clears the binding table: the record path starts a
    // fresh epoch no matter what lowering later does with the tape.
    haze::bindings().store(nth_slot_addr(0), haze::new_value_id());
    (void)haze::graph().seal();
    REQUIRE(haze::bindings().load(nth_slot_addr(0)) == haze::kUnbound);
}

TEST_CASE("Graph: reset discards the tape without lowering", "[unit][tape]") {
    TapeFixture fixture;

    haze::Node node{};
    node.kind = haze::Node::Kind::Invalidate;
    node.addr = nth_slot_addr(3);
    node.entry = "test-reset";
    haze::graph().append(std::move(node));
    REQUIRE(haze::graph().size() == 1);

    haze::graph().reset();
    REQUIRE(haze::graph().size() == 0);
}

namespace {

// Config is a process singleton too; bracket every freeze test with a
// full reset so the C-ABI test cases (which reset via hazeDeviceReset)
// never observe a frozen leftover.
struct ConfigResetFixture {
    ConfigResetFixture() { haze::config().reset(); }
    ~ConfigResetFixture() { haze::config().reset(); }
    ConfigResetFixture(const ConfigResetFixture &) = delete;
    ConfigResetFixture &operator=(const ConfigResetFixture &) = delete;
};

} // namespace

TEST_CASE("Config::freeze is a no-op before ring_dim is set", "[unit][tape]") {
    ConfigResetFixture fixture;
    REQUIRE(haze::config().freeze() == nullptr);
    REQUIRE_FALSE(haze::config().frozen());
    // The failed early freeze must not lock the user out of configuring.
    REQUIRE(haze::config().set_ring_dimension(4096).has_value());
    REQUIRE(haze::config().set_modulus(0, 576460752303415297ULL).has_value());
}

TEST_CASE("Config::freeze publishes an immutable snapshot", "[unit][tape]") {
    ConfigResetFixture fixture;
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    REQUIRE(haze::config().set_ring_dimension(4096).has_value());
    REQUIRE(haze::config().set_modulus(0, kQ0).has_value());
    REQUIRE(haze::config().set_modulus(1, kQ1).has_value());

    const haze::ConfigSnapshot *snap = haze::config().freeze();
    REQUIRE(snap != nullptr);
    REQUIRE(haze::config().frozen());
    REQUIRE(snap->ring_dim == 4096);
    REQUIRE(snap->modulus(0) == kQ0);
    REQUIRE(snap->modulus(1) == kQ1);
    REQUIRE(snap->modulus(2) == 0);  // out of range reads as unset
    REQUIRE(snap->modulus(-1) == 0); // negative index reads as unset

    // Idempotent: same snapshot object on re-freeze.
    REQUIRE(haze::config().freeze() == snap);
}

TEST_CASE("Config: post-freeze mutation gets the configure_device treatment", "[unit][tape]") {
    ConfigResetFixture fixture;
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    REQUIRE(haze::config().set_ring_dimension(4096).has_value());
    REQUIRE(haze::config().set_modulus(0, kQ0).has_value());
    REQUIRE(haze::config().freeze() != nullptr);

    // Identical re-sets keep succeeding (idempotent setters)...
    REQUIRE(haze::config().set_ring_dimension(4096).has_value());
    REQUIRE(haze::config().set_modulus(0, kQ0).has_value());
    // ...changes are rejected.
    REQUIRE_FALSE(haze::config().set_ring_dimension(8192).has_value());
    REQUIRE_FALSE(haze::config().set_modulus(0, kQ0 + 2).has_value());
    REQUIRE_FALSE(haze::config().set_modulus(1, kQ0).has_value());

    // reset() thaws everything.
    haze::config().reset();
    REQUIRE_FALSE(haze::config().frozen());
    REQUIRE(haze::config().set_ring_dimension(8192).has_value());
}

// ---------------------------------------------------------------------------
// derive(): the flush-time replay of EpochState's record-time bookkeeping.
// Each case reproduces a semantic the eager engine implemented, using
// synthetic metadata-only nodes (thunks never run under derive()).
// ---------------------------------------------------------------------------

namespace {

haze::Node input_node(haze::DevAddr addr, haze::ValueId vid) {
    haze::Node n{};
    n.kind = haze::Node::Kind::InputSnapshot;
    n.addr = addr;
    n.dst_vid = vid;
    return n;
}

haze::Node h2d_node(haze::DevAddr addr, haze::ValueId vid) {
    haze::Node n{};
    n.kind = haze::Node::Kind::H2DInput;
    n.addr = addr;
    n.dst_vid = vid;
    return n;
}

haze::Node compute_node(haze::DevAddr dst, haze::ValueId vid,
                        std::vector<haze::ValueId> srcs = {}) {
    haze::Node n{};
    n.kind = haze::Node::Kind::Compute;
    n.addr = dst;
    n.dst_vid = vid;
    n.src_vids = std::move(srcs);
    return n;
}

haze::Node tag_node(haze::DevAddr addr) {
    haze::Node n{};
    n.kind = haze::Node::Kind::TagOutput;
    n.addr = addr;
    return n;
}

haze::Node invalidate_node(haze::DevAddr addr) {
    haze::Node n{};
    n.kind = haze::Node::Kind::Invalidate;
    n.addr = addr;
    return n;
}

haze::Node mrp_register_node(std::string name, std::vector<haze::DevAddr> addrs,
                             std::vector<uint64_t> moduli) {
    haze::Node n{};
    n.kind = haze::Node::Kind::MrpRegister;
    n.addr = addrs.front();
    n.name = std::move(name);
    n.group_addrs = std::move(addrs);
    n.group_moduli = std::move(moduli);
    return n;
}

haze::Node mrp_input_tag_node(std::string name, std::vector<haze::ValueId> vids) {
    haze::Node n{};
    n.kind = haze::Node::Kind::MrpInputTag;
    n.name = std::move(name);
    n.group_vids = std::move(vids);
    return n;
}

} // namespace

TEST_CASE("derive: input nodes get first-touch-ordered haze_in names", "[unit][tape]") {
    const auto a0 = nth_slot_addr(0);
    const auto a1 = nth_slot_addr(1);
    std::vector<haze::Node> tape;
    tape.push_back(input_node(a0, 11));
    tape.push_back(h2d_node(a1, 12));

    const haze::DerivedState d = haze::derive(tape);
    REQUIRE(d.node_names[0] == "haze_in_0");
    REQUIRE(d.node_names[1] == "haze_in_1");
    REQUIRE(d.final_bindings.at(a0) == 11);
    REQUIRE(d.final_bindings.at(a1) == 12);
    REQUIRE_FALSE(d.has_outputs());
}

TEST_CASE("derive: tag-then-overwrite resolves to the final binding under the tag-time name",
          "[unit][tape]") {
    const auto a = nth_slot_addr(0);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(a, 21));
    tape.push_back(tag_node(a));
    tape.push_back(compute_node(a, 22)); // overwrite after tagging
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE(d.pending_outputs.at(a) == "haze_out_0");
    REQUIRE(d.final_bindings.at(a) == 22); // last bind wins, as poly_map_ did
}

TEST_CASE("derive: H2D re-upload erases a pending tag without refunding the name", "[unit][tape]") {
    const auto a = nth_slot_addr(0);
    const auto b = nth_slot_addr(1);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(a, 31));
    tape.push_back(tag_node(a));     // consumes haze_out_0
    tape.push_back(h2d_node(a, 32)); // re-upload: a is an input again
    tape.push_back(compute_node(b, 33, {32}));
    tape.push_back(tag_node(b)); // must get haze_out_1, not _0
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE_FALSE(d.pending_outputs.contains(a));
    REQUIRE(d.pending_outputs.at(b) == "haze_out_1");
}

TEST_CASE("derive: double-tagging the same addr is idempotent", "[unit][tape]") {
    const auto a = nth_slot_addr(0);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(a, 41));
    tape.push_back(tag_node(a));
    tape.push_back(tag_node(a));
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE(d.pending_outputs.size() == 1);
    REQUIRE(d.pending_outputs.at(a) == "haze_out_0");
}

TEST_CASE("derive: tagging one MRP residue expands to the whole registered group", "[unit][tape]") {
    const auto r0 = nth_slot_addr(0);
    const auto r1 = nth_slot_addr(1);
    const auto lone = nth_slot_addr(2);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(r0, 51));
    tape.push_back(compute_node(r1, 52));
    tape.push_back(mrp_register_node("haze_mrp_out_x", {r0, r1}, {97, 193}));
    tape.push_back(compute_node(lone, 53));
    tape.push_back(tag_node(r1)); // any residue tags the group
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE(d.pending_outputs.contains(r0));
    REQUIRE(d.pending_outputs.contains(r1));
    REQUIRE_FALSE(d.pending_outputs.contains(lone));
    REQUIRE(d.pending_mrp_groups.contains("haze_mrp_out_x"));
    const haze::PendingMrpGroup &g = d.pending_mrp_groups.at("haze_mrp_out_x");
    REQUIRE(g.addrs == std::vector<haze::DevAddr>{r0, r1});
    REQUIRE(g.moduli == std::vector<uint64_t>{97, 193});
}

TEST_CASE("derive: invalidate drops the binding, the tag, and every group naming the addr",
          "[unit][tape]") {
    const auto r0 = nth_slot_addr(0);
    const auto r1 = nth_slot_addr(1);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(r0, 61));
    tape.push_back(compute_node(r1, 62));
    tape.push_back(mrp_register_node("haze_mrp_out_y", {r0, r1}, {97, 193}));
    tape.push_back(tag_node(r0));        // group goes pending
    tape.push_back(invalidate_node(r1)); // hazeFree of one residue
    const haze::DerivedState d = haze::derive(tape);

    // The group is gone; r1's binding and tag are gone. r0's standalone
    // tag survives (invalidate only erased r1's entries), exactly as
    // EpochState::invalidate behaved.
    REQUIRE_FALSE(d.pending_mrp_groups.contains("haze_mrp_out_y"));
    REQUIRE_FALSE(d.final_bindings.contains(r1));
    REQUIRE_FALSE(d.pending_outputs.contains(r1));
    REQUIRE(d.pending_outputs.contains(r0));
}

TEST_CASE("derive: a recycled addr re-registers a fresh group after invalidate", "[unit][tape]") {
    const auto r0 = nth_slot_addr(0);
    const auto r1 = nth_slot_addr(1);
    std::vector<haze::Node> tape;
    tape.push_back(compute_node(r0, 71));
    tape.push_back(compute_node(r1, 72));
    tape.push_back(mrp_register_node("haze_mrp_out_z", {r0, r1}, {97, 193}));
    tape.push_back(invalidate_node(r0));  // free; group dropped
    tape.push_back(compute_node(r0, 73)); // recycled allocation
    tape.push_back(compute_node(r1, 74));
    tape.push_back(mrp_register_node("haze_mrp_out_z", {r0, r1}, {97, 193}));
    tape.push_back(tag_node(r0));
    const haze::DerivedState d = haze::derive(tape);

    // The re-registration after the drop must win (the eager engine's
    // try_emplace would have found the name erased by invalidate).
    REQUIRE(d.pending_mrp_groups.contains("haze_mrp_out_z"));
    REQUIRE(d.final_bindings.at(r0) == 73);
    REQUIRE(d.final_bindings.at(r1) == 74);
}

TEST_CASE("derive: MrpInputTag dedups by group name", "[unit][tape]") {
    std::vector<haze::Node> tape;
    tape.push_back(mrp_input_tag_node("haze_mrp_in_a", {81, 82}));
    tape.push_back(mrp_input_tag_node("haze_mrp_in_a", {81, 82})); // same source MRP reused
    tape.push_back(mrp_input_tag_node("haze_mrp_in_b", {83, 84}));
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE(d.emit_mrp_input[0]);
    REQUIRE_FALSE(d.emit_mrp_input[1]);
    REQUIRE(d.emit_mrp_input[2]);
}

TEST_CASE("derive: last_use tracks each value's final consumer", "[unit][tape]") {
    const auto a = nth_slot_addr(0);
    const auto b = nth_slot_addr(1);
    const auto c = nth_slot_addr(2);
    std::vector<haze::Node> tape;
    tape.push_back(input_node(a, 91));
    tape.push_back(compute_node(b, 92, {91}));
    tape.push_back(compute_node(c, 93, {91, 92})); // 91 read again here
    const haze::DerivedState d = haze::derive(tape);

    REQUIRE(d.last_use.at(91) == 2);
    REQUIRE(d.last_use.at(92) == 2);
    REQUIRE_FALSE(d.last_use.contains(93)); // never consumed
}
