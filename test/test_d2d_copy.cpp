// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// hazeMemcpy(D2D) on a compute-produced source: src has a poly_map_
// binding but no shadow_data_ entry until D2H. The shadow-only copy
// path would silently produce zeros; the contract is to emit a
// pass-through fhetch IR node so replay materializes dst correctly.

#include "integration_helpers.hpp"

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

} // namespace

TEST_CASE("hazeMemcpy(D2D) copies a compute-produced polynomial via IR", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    void *src_in = nullptr;
    void *dst_compute = nullptr;
    void *dst_copy = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst_copy, kBytes) == HAZE_SUCCESS);

    const auto residue = haze::test::make_residue(q, /*seed=*/42, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Compute op: dst_compute = src_in + src_in. This binds dst_compute
    // in poly_map_ but leaves shadow_data_[dst_compute] absent.
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, 0, nullptr) == HAZE_SUCCESS);

    // The D2D under test.
    REQUIRE(hazeMemcpy(dst_copy, dst_compute, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst_copy, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // Expected: 2 * residue[i] mod q.
    for (std::size_t i = 0; i < kRingDim; ++i) {
        const uint64_t expected = (residue[i] + residue[i]) % q;
        REQUIRE(out[i] == expected);
    }

    REQUIRE(hazeFree(dst_copy) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst_compute) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src_in) == HAZE_SUCCESS);
}

TEST_CASE("hazeMemcpy(D2D) copies a residue into an MRP-registered addr", "[integration]") {
    // D2D is a value copy: src and dst end up holding their respective polys
    // independently. Copying a single-residue poly into one member of a
    // registered MRP output group leaves the source intact, gives dst the
    // copied value, keeps the other group members at their original
    // residues, and the group itself survives.
    constexpr uint64_t kRingDimMrp = 4096;
    constexpr std::size_t kBytesMrp = kRingDimMrp * sizeof(uint64_t);
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    constexpr uint64_t kQ1 = 576460752303439873ULL;
    constexpr uint64_t kQ2 = 576460752303702017ULL;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDimMrp) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDimMrp, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const std::vector<uint64_t> base = {picked, kQ1, kQ2};
    std::vector<std::vector<uint64_t>> src_residues = {
        haze::test::make_residue(picked, /*seed=*/1, kRingDimMrp),
        haze::test::make_residue(kQ1, /*seed=*/2, kRingDimMrp),
        haze::test::make_residue(kQ2, /*seed=*/3, kRingDimMrp),
    };

    auto srcs = haze::test::allocate_and_h2d_residues(src_residues);
    auto outs = haze::test::allocate_dst_residues(3, kBytesMrp);

    // MRP op binds outs[0..2] in poly_map_ AND registers them as a group.
    REQUIRE(hazeAddMrp(outs.data(), haze::test::to_const(srcs).data(),
                       haze::test::to_const(srcs).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    // Distinguishable single-residue value: srcs[0] + 13 mod q0. Differs
    // from outs[0]'s original (2 * srcs[0]) so the test can tell the new
    // residue apart from the original group value.
    void *single = nullptr;
    REQUIRE(hazeMalloc(&single, kBytesMrp) == HAZE_SUCCESS);
    REQUIRE(hazeAddScalar(single, srcs[0], /*scalar=*/13, 0, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeMemcpy(outs[0], single, kBytesMrp, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_SUCCESS);

    std::vector<uint64_t> out_copy(kRingDimMrp);
    std::vector<uint64_t> out_mrp1(kRingDimMrp);
    std::vector<uint64_t> out_mrp2(kRingDimMrp);
    std::vector<uint64_t> out_src(kRingDimMrp);
    REQUIRE(hazeMemcpy(out_copy.data(), outs[0], kBytesMrp, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out_mrp1.data(), outs[1], kBytesMrp, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out_mrp2.data(), outs[2], kBytesMrp, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(out_src.data(), single, kBytesMrp, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);

    for (std::size_t i = 0; i < kRingDimMrp; ++i) {
        const uint64_t expected_single = (src_residues[0][i] + 13U) % picked;
        REQUIRE(out_copy[i] == expected_single);
        REQUIRE(out_src[i] == expected_single);
        REQUIRE(out_mrp1[i] == (src_residues[1][i] + src_residues[1][i]) % kQ1);
        REQUIRE(out_mrp2[i] == (src_residues[2][i] + src_residues[2][i]) % kQ2);
    }

    REQUIRE(hazeFree(single) == HAZE_SUCCESS);
    haze::test::free_all_residues(outs);
    haze::test::free_all_residues(srcs);
}

TEST_CASE("hazeMemcpy(D2D) from a never-written source returns SOURCE_UNAVAILABLE",
          "[integration]") {
    haze::test::setup_integration_compute_config();

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    // src is allocated but never H2D'd or compute-touched. D2D must fail
    // loudly rather than silently produce a zero-filled dst.
    REQUIRE(hazeMemcpy(dst, src, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_ERROR_SOURCE_UNAVAILABLE);

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
}

TEST_CASE("hazeMemcpy(D2D) promotes an H2D'd source through the IR copy", "[integration]") {
    // H2D'd-but-never-compute-touched source: D2D triggers lookup_or_create
    // which promotes the shadow bytes into a tagged fhetch input, then
    // emits the copy IR. Round-trip bytes must match the original H2D.
    const uint64_t q = haze::test::setup_integration_compute_config();
    (void)q;

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const auto residue = haze::test::make_residue(q, /*seed=*/7, kRingDim);
    REQUIRE(hazeMemcpy(src, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(dst, src, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t i = 0; i < kRingDim; ++i) {
        REQUIRE(out[i] == residue[i]);
    }

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
}
