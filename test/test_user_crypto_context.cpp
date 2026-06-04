// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Integration test that an MRP add round-trips over a chain of primes pulled
// from a user-built CC with a non-default scaling technique (FIXEDAUTO). The
// limb-level replay is scaling-technique-agnostic, so haze's own pure-C Init
// context replays a FIXEDAUTO-derived chain unchanged. No KeyPair is
// constructed: the bridge synthesizes CT shells via SetElements, no Encrypt.

#include "integration_helpers.hpp"
#include "integration_introspect.hpp"
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

inline uint64_t add_mod(uint64_t a, uint64_t b, uint64_t q) {
    const uint64_t s = a + b;
    return (s >= q) ? s - q : s;
}

} // namespace

TEST_CASE("user CC primes: hazeAddMrp round-trips through FIXEDAUTO", "[integration]") {
    using namespace lbcrypto;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);

    // Caller-built CC with a non-default scaling technique (FIXEDAUTO) and a
    // chain deeper than the 3-residue MRP add below, so the primes come from a
    // realistic multi-level context rather than hand-picked toy values.
    CCParams<CryptoContextCKKSRNS> params;
    params.SetSecurityLevel(HEStd_NotSet);
    params.SetRingDim(static_cast<uint32_t>(kRingDim));
    params.SetMultiplicativeDepth(4); // 5-tower chain; the add below uses 3
    params.SetFirstModSize(60);
    params.SetScalingModSize(59);
    params.SetScalingTechnique(FIXEDAUTO);
    auto cc = GenCryptoContext(params);
    REQUIRE(cc);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    // Pull OpenFHE's picked primes out of the user CC and seed haze with them
    // via the pure-C bridge (scalars only); the trace and templates then use the
    // same moduli regardless of the CC's scaling technique.
    const auto &eparams = cc->GetCryptoParameters()->GetElementParams()->GetParams();
    REQUIRE(eparams.size() >= 3);
    std::vector<uint64_t> base;
    base.reserve(3);
    for (std::size_t i = 0; i < 3; ++i)
        base.push_back(eparams[i]->GetModulus().ConvertToInt());

    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, base.front(), &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);
    for (std::size_t i = 0; i < 3; ++i)
        REQUIRE(hazeSetCiphertextModulus(static_cast<int>(i), base[i]) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    // Build per-residue inputs + expected output for hazeAddMrp.
    std::vector<std::vector<uint64_t>> a(3);
    std::vector<std::vector<uint64_t>> b(3);
    std::vector<std::vector<uint64_t>> expected(3);
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

    // MRP read path: fhetch::result(name, MRP&) should report the same
    // per-residue values as the SRP D2H above, under the user CC's moduli.
    haze::test::check_mrp_against_per_residue(base, expected);

    haze::test::free_all_residues(da);
    haze::test::free_all_residues(db);
    haze::test::free_all_residues(dst);
}
