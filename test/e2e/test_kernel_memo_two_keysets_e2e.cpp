// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// One memoized kernel template, two ciphertext/key sets: the kernel key
// covers name + shapes + moduli — NEVER buffer contents — so a second
// call with a different ciphertext AND different (real, OpenFHE-
// generated) key material must be a cache hit (HAZE_KERNEL_REPLAY), and
// its flush must compute on the second call's inputs.
//
// The kernel body is one hybrid-keyswitch digit step (digit × key-row
// pointwise products in EVAL form) — the key-consuming inner loop of
// ops.cpp::hybrid_keyswitch. The FULL rotation can't be bracketed yet:
// the v1 closed-body rule has no body-local temporaries, and keyswitch
// needs scratch buffers (see the deferred list in the walkthrough doc).
// Each call flushes its own epoch, so this also covers template reuse
// ACROSS flushes (the record→replay-with-new-inputs workflow), not just
// within one recording.

#include "openfhe.h"
#include "openfhe_key_extract.hpp"
#include "ops.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

namespace {

namespace ops = haze::test::ops;

uint64_t mulmod(uint64_t a, uint64_t b, uint64_t q) {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) % q);
}

// Host copy of one ciphertext element's tower chain (EVAL-form raw
// residues, exactly the bytes ops::h2d_ct would upload).
std::vector<std::vector<uint64_t>> element_chain(const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct,
                                                 std::size_t elem_idx, uint64_t ring_dim) {
    const auto &elem = ct->GetElements()[elem_idx];
    const std::size_t towers = elem.GetNumOfElements();
    std::vector<std::vector<uint64_t>> chain(towers);
    for (std::size_t t = 0; t < towers; ++t) {
        const auto &vals = elem.GetElementAtIndex(static_cast<usint>(t)).GetValues();
        REQUIRE(vals.GetLength() == ring_dim);
        chain[t].resize(ring_dim);
        for (std::size_t i = 0; i < ring_dim; ++i)
            chain[t][i] = vals[i].ConvertToInt();
    }
    return chain;
}

// Digit-0 key rows trimmed to the first `towers` Q-primes.
std::vector<std::vector<uint64_t>>
key_rows(const std::vector<std::vector<std::vector<uint64_t>>> &limbs, std::size_t towers) {
    REQUIRE(!limbs.empty());
    REQUIRE(limbs[0].size() >= towers);
    return {limbs[0].begin(), limbs[0].begin() + static_cast<std::ptrdiff_t>(towers)};
}

// One ciphertext/key set, host + device side by side.
struct KeysetCall {
    std::vector<std::vector<uint64_t>> dig_host;
    std::vector<std::vector<uint64_t>> key_a_host;
    std::vector<std::vector<uint64_t>> key_b_host;
    ops::Allocs dig;
    ops::Allocs key_a;
    ops::Allocs key_b;
    ops::Allocs out_a;
    ops::Allocs out_b;
};

KeysetCall upload_set(const ops::OpCtx &ctx, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct,
                      const haze::test::HybridKeyswitchLimbs &limbs, std::size_t towers) {
    KeysetCall c;
    c.dig_host = element_chain(ct, 1, ctx.ring_dim);
    REQUIRE(c.dig_host.size() == towers);
    c.key_a_host = key_rows(limbs.a_limbs, towers);
    c.key_b_host = key_rows(limbs.b_limbs, towers);
    c.dig = ops::Allocs(c.dig_host);
    c.key_a = ops::Allocs(c.key_a_host);
    c.key_b = ops::Allocs(c.key_b_host);
    c.out_a = ops::Allocs(towers, ctx.poly_bytes);
    c.out_b = ops::Allocs(towers, ctx.poly_bytes);
    return c;
}

// Bracketed "keyswitch digit" kernel call; the body runs only on
// RECORD. Returns the disposition Begin reported.
hazeKernelDisposition ks_digit(KeysetCall &c, const std::vector<uint64_t> &base) {
    const std::vector<const void *> dig_c = c.dig.as_const();
    const std::vector<const void *> key_a_c = c.key_a.as_const();
    const std::vector<const void *> key_b_c = c.key_b.as_const();
    const hazeKernelInput inputs[] = {
        {dig_c.data(), base.data(), base.size()},
        {key_a_c.data(), base.data(), base.size()},
        {key_b_c.data(), base.data(), base.size()},
    };
    // Shape/parameter key: the moduli base. Ciphertext and key CONTENTS
    // never enter the key — that is the property under test.
    std::vector<uint8_t> key_bytes(base.size() * sizeof(uint64_t));
    std::memcpy(key_bytes.data(), base.data(), key_bytes.size());

    hazeKernelDisposition disposition{};
    REQUIRE(hazeKernelBegin("ks_digit", 0xD161, key_bytes.data(), key_bytes.size(), inputs, 3,
                            &disposition) == HAZE_SUCCESS);
    if (disposition == HAZE_KERNEL_RECORD) {
        REQUIRE(hazeMulMrp(c.out_a.data(), dig_c.data(), key_a_c.data(), base.data(), base.size(),
                           nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(c.out_b.data(), dig_c.data(), key_b_c.data(), base.data(), base.size(),
                           nullptr) == HAZE_SUCCESS);
    }
    const hazeKernelOutput outputs[] = {
        {c.out_a.data(), base.data(), base.size()},
        {c.out_b.data(), base.data(), base.size()},
    };
    REQUIRE(hazeKernelEnd(outputs, 2) == HAZE_SUCCESS);
    return disposition;
}

// Flush, read both outputs back, and check every residue is the
// pointwise product of THIS call's digit and key rows.
void flush_and_verify(const ops::OpCtx &ctx, const KeysetCall &c,
                      const std::vector<uint64_t> &base) {
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    const auto check = [&](const ops::Allocs &out, const std::vector<std::vector<uint64_t>> &key) {
        for (std::size_t t = 0; t < base.size(); ++t) {
            std::vector<uint64_t> got(ctx.ring_dim);
            REQUIRE(hazeMemcpy(got.data(), out[t], ctx.poly_bytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                    HAZE_SUCCESS);
            for (std::size_t i = 0; i < ctx.ring_dim; ++i)
                REQUIRE(got[i] == mulmod(c.dig_host[t][i], key[t][i], base[t]));
        }
    };
    check(c.out_a, c.key_a_host);
    check(c.out_b, c.key_b_host);
}

} // namespace

TEST_CASE("memoized kernel replays a second ciphertext/key set across flushes",
          "[integration][e2e][kernelmemo]") {
    using namespace lbcrypto;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetKernelMemo(1) == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = FIXEDMANUAL,
                              .mult_depth = 1,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .with_relin_key = false,
                              .rotate_indices = {1},
                              .ring_dim = ops::RingDimChoice::OpenFHEDerives()});

    // Keyset A: make_ctx's keypair + its extracted automorphism key.
    // Keyset B: a SECOND keypair on the same CryptoContext — same scheme
    // parameters (so the same kernel key), different secret/public key
    // and therefore different ciphertext and key-switch limbs.
    const std::uint32_t auto_index = ctx.rotation_keys.at(1).auto_index;
    auto keys_b = ctx.cc->KeyGen();
    ctx.cc->EvalAtIndexKeyGen(keys_b.secretKey, {1});
    haze::test::HybridKeyswitchLimbs limbs_b;
    REQUIRE(haze::test::extract_automorphism_key_limbs(ctx.cc, keys_b.secretKey, auto_index,
                                                       limbs_b) == HAZE_SUCCESS);
    REQUIRE(limbs_b.q_base == ctx.q_base);

    const std::vector<double> x_a = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> x_b = {5.0, 4.0, 3.0, 2.0, 1.0, 0.75, 0.5, 0.25};
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, ctx.cc->MakeCKKSPackedPlaintext(x_a));
    auto ct_b = ctx.cc->Encrypt(keys_b.publicKey, ctx.cc->MakeCKKSPackedPlaintext(x_b));

    const std::size_t towers = ct_a->GetElements()[1].GetNumOfElements();
    REQUIRE(towers >= 2); // multi-residue, so the MRP group machinery is exercised
    const std::vector<uint64_t> base(ctx.q_base.begin(),
                                     ctx.q_base.begin() + static_cast<std::ptrdiff_t>(towers));

    // Call 1 (set A): cold — records the template, flushes its epoch.
    KeysetCall call_a = upload_set(ctx, ct_a, ctx.rotation_keys.at(1).limbs, towers);
    REQUIRE(ks_digit(call_a, base) == HAZE_KERNEL_RECORD);
    flush_and_verify(ctx, call_a, base);

    // Call 2 (set B): different ciphertext, different keys — the cache
    // hit is REQUIRED, and the replayed instance must compute on B's
    // inputs in a fresh epoch.
    KeysetCall call_b = upload_set(ctx, ct_b, limbs_b, towers);
    REQUIRE(ks_digit(call_b, base) == HAZE_KERNEL_REPLAY);
    flush_and_verify(ctx, call_b, base);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
