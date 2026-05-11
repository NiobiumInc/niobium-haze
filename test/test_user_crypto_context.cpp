// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Integration test for the bridge's user-provided CryptoContext path.
// Builds a CC with a non-default scaling technique (FIXEDAUTO) and feeds
// it to the bridge via hazeReplayBridgeRegisterCryptoContext, then runs an
// MRP add and verifies the per-residue result via D2H.

#include "integration_helpers.hpp"
#include "openfhe.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <haze/replay_bridge_cc.hpp>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

inline uint64_t add_mod(uint64_t a, uint64_t b, uint64_t q) {
    const uint64_t s = a + b;
    return (s >= q) ? s - q : s;
}

} // namespace

TEST_CASE("user-registered CC: hazeAddMrp round-trips through FIXEDAUTO", "[integration]") {
    using namespace lbcrypto;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);

    // Caller-built CC with a scaling technique the bridge's default path
    // (hazeReplayBridgeInitCryptoContext) would force to FIXEDMANUAL.
    CCParams<CryptoContextCKKSRNS> params;
    params.SetSecurityLevel(HEStd_NotSet);
    params.SetRingDim(static_cast<uint32_t>(kRingDim));
    params.SetMultiplicativeDepth(2); // 3-tower chain
    params.SetFirstModSize(60);
    params.SetScalingModSize(59);
    params.SetScalingTechnique(FIXEDAUTO);
    auto cc = GenCryptoContext(params);
    REQUIRE(cc);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);
    auto keys = cc->KeyGen();
    REQUIRE(keys.publicKey);
    REQUIRE(keys.secretKey);

    REQUIRE(haze::hazeReplayBridgeRegisterCryptoContext(cc, keys) == HAZE_SUCCESS);

    // Pull OpenFHE's picked primes out of the CC and seed haze's modulus
    // table with them so the trace and the templates use the same moduli.
    const auto &eparams = cc->GetCryptoParameters()->GetElementParams()->GetParams();
    REQUIRE(eparams.size() >= 3);
    std::vector<uint64_t> base;
    base.reserve(3);
    for (std::size_t i = 0; i < 3; ++i) {
        base.push_back(eparams[i]->GetModulus().ConvertToInt());
        REQUIRE(hazeSetCiphertextModulus(static_cast<int>(i), base[i]) == HAZE_SUCCESS);
    }
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    // Build per-residue inputs + expected output for hazeAddMrp.
    std::vector<std::vector<uint64_t>> a(3), b(3), expected(3);
    for (std::size_t i = 0; i < 3; ++i) {
        a[i] = haze::test::make_residue(base[i], 0xAAAAULL + i, kRingDim);
        b[i] = haze::test::make_residue(base[i], 0xBBBBULL + i, kRingDim);
        expected[i].resize(kRingDim);
        for (std::size_t k = 0; k < kRingDim; ++k)
            expected[i][k] = add_mod(a[i][k], b[i][k], base[i]);
    }

    auto da = haze::test::allocate_and_h2d_residues(a);
    auto db = haze::test::allocate_and_h2d_residues(b);
    auto dst = haze::test::allocate_dst_residues(3, kBytes);

    auto da_const = haze::test::to_const(da);
    auto db_const = haze::test::to_const(db);
    REQUIRE(hazeAddMrp(dst.data(), da_const.data(), db_const.data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    for (std::size_t i = 0; i < 3; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (std::size_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " (mod " << base[i] << ") slot " << k);
            REQUIRE(got[k] == expected[i][k]);
        }
    }

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}
