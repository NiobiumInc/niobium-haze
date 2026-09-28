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
//
// hazeReplayBridgeReset releases the CryptoContexts the bridge registered in
// OpenFHE's process-wide factory, and the bridge rebuilds what a later flush needs.

#include "integration_helpers.hpp"
#include "openfhe.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

int context_count() {
    return lbcrypto::CryptoContextFactory<lbcrypto::DCRTPoly>::GetContextCount();
}

// x + x recorded over `base` and tagged, not yet flushed.
struct PendingAdd {
    std::vector<std::vector<uint64_t>> x;
    std::vector<void *> src;
    std::vector<void *> dst;
};

PendingAdd record_add(const std::vector<uint64_t> &base, uint64_t seed) {
    PendingAdd add{.x = std::vector<std::vector<uint64_t>>(base.size()), .src = {}, .dst = {}};
    for (std::size_t i = 0; i < base.size(); ++i)
        add.x[i] = haze::test::make_residue(base[i], seed + i, kRingDim);
    add.src = haze::test::allocate_and_h2d_residues(add.x, base);
    add.dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const auto src_const = haze::test::to_const(add.src);
    REQUIRE(hazeAddMrp(add.dst.data(), src_const.data(), src_const.data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : add.dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    return add;
}

// Flush, and check every residue of `add` exactly.
void flush_and_check(const PendingAdd &add, const std::vector<uint64_t> &base) {
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> out(kRingDim);
        REQUIRE(hazeMemcpy(out.data(), add.dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        std::vector<uint64_t> expected(kRingDim);
        for (std::size_t j = 0; j < kRingDim; ++j)
            expected[j] = haze::test::add_mod(add.x[i][j], add.x[i][j], base[i]);
        CAPTURE(i);
        REQUIRE(out == expected);
    }
    haze::test::free_all_residues(add.src);
    haze::test::free_all_residues(add.dst);
}

void add_round_trip(const std::vector<uint64_t> &base, uint64_t seed) {
    flush_and_check(record_add(base, seed), base);
}

} // namespace

TEST_CASE("hazeReplayBridgeReset releases every cached CryptoContext", "[integration]") {
    const auto base = haze::test::setup_integration_mrp3_config(kRingDim);
    add_round_trip(base, 0x2000ULL);
    // The init's single-prime context plus the context the hook built for the
    // three-prime input shape.
    const int before = context_count();
    CAPTURE(before);
    REQUIRE(before > 1);

    hazeReplayBridgeReset();
    REQUIRE(context_count() == 0);

    haze::test::setup_integration_mrp3_config(kRingDim);
    add_round_trip(base, 0x3000ULL);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(context_count() == 0);
}

// The bridge stays armed across its own reset: a recording flushed after it must
// synthesize fresh inputs rather than replay the previous epoch's artifacts.
TEST_CASE("a flush after hazeReplayBridgeReset returns the new recording's values",
          "[integration]") {
    const auto base = haze::test::setup_integration_mrp3_config(kRingDim);
    add_round_trip(base, 0x4000ULL);
    hazeReplayBridgeReset();
    add_round_trip(base, 0x5000ULL);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// The same holds for a reset between tagging and flushing: the pending recording's
// inputs are still synthesized from their spill files at the flush.
TEST_CASE("a flush after hazeReplayBridgeReset mid-recording returns its values", "[integration]") {
    const auto base = haze::test::setup_integration_mrp3_config(kRingDim);
    add_round_trip(base, 0x6000ULL);
    const PendingAdd add = record_add(base, 0x7000ULL);
    hazeReplayBridgeReset();
    flush_and_check(add, base);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
