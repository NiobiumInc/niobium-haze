// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Unit tests for the deferred-recording tape primitives: the global
// ValueId counter, the lock-free BindingTable (addr -> ValueId), and the
// Graph append/seal/reset lifecycle. These are internal-API tests
// (haze_tests links the haze object files directly), tagged [unit] so
// they run in the `make test-unit` sweep.

#include "common/handle.hpp"
#include "core/graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
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
