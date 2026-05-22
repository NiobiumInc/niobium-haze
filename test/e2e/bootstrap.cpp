// Copyright (C) 2026, All rights reserved by Niobium Microsystems.

#include "bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <openfhe.h>
#include <scheme/ckksrns/ckksrns-cryptoparameters.h>
#include <stdexcept>

namespace haze::test::ops {

namespace {

// Cumulative-rotation sum used by OpenFHE's sparsely-packed bootstrap
// before CtS (ckksrns-fhe.cpp:771-773). Each j*slots rotation key is
// in bk.rotation_keys via EvalBootstrapKeyGen.
Ct partial_sum(const OpCtx &ctx, const BootstrapKeys &bk, Ct ct) {
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const std::uint32_t limit = N / (2 * bk.params.slots);
    for (std::uint32_t j = 1; j < limit; j <<= 1) {
        const std::uint32_t auto_index = ctx.cc->FindAutomorphismIndex(j * bk.params.slots);
        auto it = bk.rotation_keys.find(auto_index);
        REQUIRE(it != bk.rotation_keys.end());
        Ct rotated = rotate_with_key(ctx, ct, it->second);
        ct = add(ctx, ct, rotated);
    }
    return ct;
}

} // namespace

Ct bootstrap(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct, BootstrapVariant variant) {
    switch (variant) {
    case BootstrapVariant::Standard: {
        // Reduce input to a single tower for mod_raise. (CKKS-valid: we
        // only consume tower 0 in mod_raise; the rest are ignored.)
        Ct depleted = clone_ct(ctx, ct);
        if (depleted.towers() > 1)
            depleted = level_reduce(ctx, std::move(depleted), depleted.towers() - 1);
        Ct raised = mod_raise(ctx, bk, depleted);

        // Mirrors ckksrns-fhe.cpp:666 — scale by pre/(k*N). Without this, the
        // post-mod_raise NSD=1 makes the subsequent rescale underflow to 0.
        //
        // NOTE: OpenFHE also has an AdjustCiphertext step (ckksrns-fhe.cpp:590)
        // BEFORE mod_raise that EvalMults by 2^-correction. Adding it here
        // consumes one extra level that breaks the precomputed CtS plaintext
        // matrices' size expectation (they're sized for OpenFHE's exact
        // pipeline). Reconciling this is part of the e2e gap.
        auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
            ctx.cc->GetCryptoParameters());
        REQUIRE(cp);
        const double qDouble =
            cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
        const double powP = std::pow(2.0, cp->GetPlaintextModulus());
        const std::int32_t deg =
            static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
        const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
        constexpr std::uint32_t K_UNIFORM = 512;
        const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
        const double pre_factor = pre / (static_cast<double>(K_UNIFORM) * N);
        raised = eval_mult_scalar_for_test(ctx, raised, pre_factor);

        if (bk.params.slots < N / 2)
            raised = partial_sum(ctx, bk, std::move(raised));

        raised = rescale(ctx, raised);
        Ct in_slots = linear_transform(ctx, bk, bk.cts_matrices, raised);
        Ct modded = eval_mod(ctx, bk, in_slots);
        // Defensive: with the right double_angle_iterations count for the
        // secret-key dist (set in make_bootstrap_keys), modded.towers should
        // already match stc_q_towers — the level_reduce here is a no-op.
        // Left in as a guard for parameter combinations that consume fewer
        // levels; remove when ops::bootstrap is mode-aware enough that the
        // condition becomes unreachable.
        const std::size_t stc_q_towers =
            bk.stc_matrices.front().size() - ctx.p_base.size();
        if (modded.towers() > stc_q_towers)
            modded = level_reduce(ctx, std::move(modded), modded.towers() - stc_q_towers);
        Ct out = linear_transform(ctx, bk, bk.stc_matrices, modded);

        // SPARSELY PACKED post-StC: ctxtDec += rot(ctxtDec, slots), then
        // multiply by corFactor = 2^11. Mirrors ckksrns-fhe.cpp:846, 852.
        if (bk.params.slots < N / 2) {
            const std::uint32_t auto_index = ctx.cc->FindAutomorphismIndex(bk.params.slots);
            auto it = bk.rotation_keys.find(auto_index);
            REQUIRE(it != bk.rotation_keys.end());
            Ct rotated = rotate_with_key(ctx, out, it->second);
            out = add(ctx, out, rotated);
        }
        constexpr std::uint64_t corFactor = 1ULL << 11; // correctionFactor=11
        out = mult_int_scalar_for_test(ctx, out, corFactor);
        return out;
    }
    case BootstrapVariant::StCFirst: {
        Ct in_coeffs = linear_transform(ctx, bk, bk.stc_matrices, ct);
        Ct raised = mod_raise(ctx, bk, in_coeffs);
        Ct in_slots = linear_transform(ctx, bk, bk.cts_matrices, raised);
        return eval_mod(ctx, bk, in_slots);
    }
    }
    throw std::logic_error("haze::test::ops::bootstrap: unhandled BootstrapVariant");
}

} // namespace haze::test::ops
