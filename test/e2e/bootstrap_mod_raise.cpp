// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// CKKS modulus raising: extend a single-tower ciphertext (at q_0) to the
// full Q chain via basis extension, mirroring the UNIFORM_TERNARY path of
// FHECKKSRNS::EvalBootstrap's ModRaise section (ckksrns-fhe.cpp:619-628).

#include "bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

namespace haze::test::ops {

namespace {

// Extend one polynomial in coefficient form from {q_0} to full Q.
// Wraps hazeBasisConvert.
Allocs extend_one(const OpCtx &ctx, const Allocs &src_q0, const std::vector<uint64_t> &q_full) {
    std::vector<uint64_t> q0 = {ctx.q_base[0]};
    const hazeBasisConvertParams params = {
        .src_base = q0.data(),
        .src_base_len = q0.size(),
        .dst_base = q_full.data(),
        .dst_base_len = q_full.size(),
    };
    Allocs out(q_full.size(), ctx.poly_bytes);
    REQUIRE(hazeBasisConvert(out.data(), src_q0.as_const().data(), &params, nullptr) ==
            HAZE_SUCCESS);
    return out;
}

} // namespace

Ct mod_raise(const OpCtx &ctx, const BootstrapKeys & /*bk*/, const Ct &ct) {
    // Input contract for the simple UNIFORM_TERNARY path: ct is at a
    // single Q-tower (q_0) in EVALUATION form. INTT, extend the single
    // residue to the full Q chain, NTT back.
    REQUIRE(ct.towers() == 1);

    const std::vector<uint64_t> q_full = ctx.q_base;
    std::vector<uint64_t> q0_base = {ctx.q_base[0]};

    Allocs c0_coeff(1, ctx.poly_bytes);
    Allocs c1_coeff(1, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(c0_coeff.data(), ct.c0().as_const().data(), q0_base.data(), q0_base.size(),
                        nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTTMrp(c1_coeff.data(), ct.c1().as_const().data(), q0_base.data(), q0_base.size(),
                        nullptr) == HAZE_SUCCESS);

    Allocs c0_extended = extend_one(ctx, c0_coeff, q_full);
    Allocs c1_extended = extend_one(ctx, c1_coeff, q_full);

    Allocs c0_eval(q_full.size(), ctx.poly_bytes);
    Allocs c1_eval(q_full.size(), ctx.poly_bytes);
    REQUIRE(hazeNTTMrp(c0_eval.data(), c0_extended.as_const().data(), q_full.data(), q_full.size(),
                       nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeNTTMrp(c1_eval.data(), c1_extended.as_const().data(), q_full.data(), q_full.size(),
                       nullptr) == HAZE_SUCCESS);

    // Mirrors OpenFHE mod-raise (ckksrns-fhe.cpp:620-628 + SetLevel at 628):
    // SF preserved (re-embed is value-preserving across the new chain),
    // level = L0 - new_tower_count = 0 since we re-embed into the full chain.
    return Ct{std::move(c0_eval), std::move(c1_eval), q_full.size(),
              ct.noise_scale_deg(), ct.scaling_factor(), 0u};
}

} // namespace haze::test::ops
