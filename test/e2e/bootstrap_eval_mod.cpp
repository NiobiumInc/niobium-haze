// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// CKKS approximate modular reduction via Chebyshev series evaluation
// (Paterson-Stockmeyer) + double-angle iterations + final scaling.
// Mirrors niobium's customized OpenFHE InnerEvalChebyshevPS_NB
// (ckksrns-advancedshe.cpp:666). All ciphertext-level ops use existing
// haze ops::* primitives.

#include "bootstrap.hpp"
#include "ops.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <memory>
#include <niobium/compiler.h>
#include <openfhe.h>
#include <scheme/ckksrns/ckksrns-fhe.h>
#include <scheme/ckksrns/ckksrns-utils.h>
#include <vector>

namespace haze::test::ops {

namespace {

// Encode a constant value at the level matching `ref_ct` and h2d it.
// The OpenFHE encoding work (MakeCKKSPackedPlaintext + SetFormat) is CPROBES-
// instrumented and would emit stray sr_* IR into the active haze recording
// — niobium-haze phasefs05 / phasefs14 reproduce the resulting Cheby
// divergence. Wrap the OpenFHE-touching block in PausedRecording so the
// host-only encoding stays out of the trace; the extracted residues land in
// haze memory via Allocs's HOST_TO_DEVICE memcpy, which is the only IR-
// emitting step we want.
Allocs encode_const_pt(const OpCtx &ctx, const Ct &ref_ct, double scalar,
                       std::uint32_t noise_scale_deg) {
    using namespace lbcrypto;
    const std::uint32_t level = static_cast<std::uint32_t>(ctx.q_base.size() - ref_ct.towers());
    std::vector<std::vector<std::uint64_t>> chain(ref_ct.towers());
    {
        struct PausedRec {
            PausedRec() noexcept { ::niobium::compiler().pause(); }
            ~PausedRec() noexcept { ::niobium::compiler().resume(); }
        } _pause;
        Plaintext pt =
            ctx.cc->MakeCKKSPackedPlaintext(std::vector<std::complex<double>>(
                                                ctx.cc->GetEncodingParams()->GetBatchSize(),
                                                std::complex<double>(scalar, 0)),
                                            noise_scale_deg, level);
        auto pt_elem = pt->GetElement<DCRTPoly>();
        pt_elem.SetFormat(Format::EVALUATION);
        for (std::size_t t = 0; t < ref_ct.towers(); ++t) {
            const auto &np = pt_elem.GetElementAtIndex(static_cast<usint>(t));
            const auto &vals = np.GetValues();
            chain[t].resize(ctx.ring_dim);
            for (std::size_t i = 0; i < ctx.ring_dim; ++i)
                chain[t][i] = vals[i].template ConvertToInt<std::uint64_t>();
        }
    }
    return Allocs(chain);
}

// Mirror OpenFHE's LeveledSHECKKSRNS::EvalMultCoreInPlace via the
// GetElementForEvalMult per-tower factor algorithm
// (ckksrns-leveledshe.cpp:441-506). Bumps NSD by 1, no rescale.
//
// Importantly, EvalMult does NOT route through MakeCKKSPackedPlaintext
// (the slot-FFT encoder) — it builds per-tower integer factors directly
// from the scalar and the moduli. For negative scalars these encodings
// diverge from the FFT path; phase 16 catches it. So this helper builds
// the same per-tower factors and uses hazeMulScalarMrp.
Ct mult_by_const(const OpCtx &ctx, const Ct &ct, double scalar) {
    auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const std::size_t towers = ct.towers();
    const std::uint32_t level =
        static_cast<std::uint32_t>(ctx.q_base.size() - towers);
    const double scFactor = cp->GetScalingFactorReal(level);

    // Simplified branch of GetElementForEvalMult: assumes the scaled
    // constant fits in int64 (true for our test scalars at scFactor≈2^50).
    // Match OpenFHE's `+ 0.5` rounding semantics — truncation toward zero
    // after the offset.
    const std::int64_t large =
        static_cast<std::int64_t>(scalar * scFactor + 0.5);

    std::vector<std::uint64_t> factors(towers);
    for (std::size_t t = 0; t < towers; ++t) {
        const std::uint64_t q_t = ctx.q_base[t];
        std::int64_t reduced = large % static_cast<std::int64_t>(q_t);
        if (reduced < 0)
            reduced += static_cast<std::int64_t>(q_t);
        factors[t] = static_cast<std::uint64_t>(reduced);
    }

    std::vector<std::uint64_t> base(ctx.q_base.begin(),
                                    ctx.q_base.begin() +
                                        static_cast<std::ptrdiff_t>(towers));
    Allocs out_c0(towers, ctx.poly_bytes);
    Allocs out_c1(towers, ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out_c0.data(), ct.c0().as_const().data(),
                             factors.data(), base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalarMrp(out_c1.data(), ct.c1().as_const().data(),
                             factors.data(), base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    // Mirrors LeveledSHECKKSRNS::EvalMultCoreInPlace: SF *= scFactor at level.
    return Ct{std::move(out_c0), std::move(out_c1), towers,
              ct.noise_scale_deg() + 1,
              ct.scaling_factor() * scFactor, ct.level()};
}

// Mirror cc->EvalMult(ct, double) — the user-facing wrapper. For
// FIXEDAUTO with input NSD=2, OpenFHE's EvalMultInPlace first
// ModReduceInternalInPlace (drops level + NSD), then EvalMultCoreInPlace
// (bumps NSD back to 2). Net: same NSD, -1 tower. Use this in code that
// mirrors a `cc->EvalMult(ct, scalar)` call (AccumBabyStep, the nc==1 cu
// path); use plain mult_by_const for AdjustLevelsAndDepth-style NSD bumps.
Ct eval_mult_scalar(const OpCtx &ctx, const Ct &ct, double scalar) {
    if (ctx.mode != lbcrypto::FIXEDMANUAL && ct.noise_scale_deg() == 2) {
        Ct rescaled = rescale(ctx, ct);
        return mult_by_const(ctx, rescaled, scalar);
    }
    return mult_by_const(ctx, ct, scalar);
}

// EvalAdd by a double constant. The c1 component is unchanged, so we
// reproduce it via mult_int_scalar(c1, 1) — D2D on a trace-output Allocs
// doesn't replay correctly (the simulator can't read its bytes back),
// but a compute op like mult-by-1 replays as the identity polynomial.
Ct add_const(const OpCtx &ctx, const Ct &ct, double scalar) {
    Allocs pt = encode_const_pt(ctx, ct, scalar, ct.noise_scale_deg());
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(ct.towers()));
    Allocs out_c0(ct.towers(), ctx.poly_bytes);
    REQUIRE(hazeAddMrp(out_c0.data(), ct.c0().as_const().data(), pt.as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    std::vector<std::uint64_t> ones(ct.towers(), 1);
    Allocs out_c1(ct.towers(), ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out_c1.data(), ct.c1().as_const().data(), ones.data(),
                             base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    // EvalAddInPlace by plaintext doesn't change SF/level.
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg(),
              ct.scaling_factor(), ct.level()};
}

// Multiply each tower by a fixed uint64 scalar broadcast across all slots.
Ct mult_int_scalar(const OpCtx &ctx, const Ct &ct, std::uint64_t scalar) {
    std::vector<uint64_t> scalars(ct.towers(), scalar);
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(ct.towers()));
    Allocs out_c0(ct.towers(), ctx.poly_bytes);
    Allocs out_c1(ct.towers(), ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out_c0.data(), ct.c0().as_const().data(), scalars.data(),
                             base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalarMrp(out_c1.data(), ct.c1().as_const().data(), scalars.data(),
                             base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    // MultByIntegerInPlace: SF/NSD/level preserved (integer scalar).
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg(),
              ct.scaling_factor(), ct.level()};
}

// OpenFHE's MultByMonomialInPlace.
// The OpenFHE-side monomial construction (NativePoly + SetFormat) emits stray
// CPROBES IR into the active haze recording — see encode_const_pt above for
// the same pattern. Wrap in PausedRecording so only the hazeMulMrp below is
// in the trace.
Ct mult_monomial(const OpCtx &ctx, const Ct &ct, std::uint32_t power) {
    using namespace lbcrypto;
    const std::uint32_t M = static_cast<std::uint32_t>(2 * ctx.ring_dim);
    std::vector<std::vector<std::uint64_t>> chain(ct.towers());
    {
        struct PausedRec {
            PausedRec() noexcept { ::niobium::compiler().pause(); }
            ~PausedRec() noexcept { ::niobium::compiler().resume(); }
        } _pause;
        auto params = std::dynamic_pointer_cast<CryptoParametersCKKSRNS>(ctx.cc->GetCryptoParameters());
        auto fullParams = params->GetElementParams();
        auto paramsNative = fullParams->GetParams()[0];
        NativePoly monomial(paramsNative, Format::COEFFICIENT, true);
        const std::uint32_t powerReduced = power % M;
        monomial[power % ctx.ring_dim] =
            powerReduced < ctx.ring_dim ? NativeInteger(1)
                                        : paramsNative->GetModulus() - NativeInteger(1);
        DCRTPoly monomialDCRT(fullParams, Format::COEFFICIENT, true);
        monomialDCRT = monomial;
        monomialDCRT.SetFormat(Format::EVALUATION);

        for (std::size_t t = 0; t < ct.towers(); ++t) {
            const auto &np = monomialDCRT.GetElementAtIndex(static_cast<usint>(t));
            const auto &vals = np.GetValues();
            chain[t].resize(ctx.ring_dim);
            for (std::size_t i = 0; i < ctx.ring_dim; ++i)
                chain[t][i] = vals[i].template ConvertToInt<std::uint64_t>();
        }
    }
    Allocs mono_chain(chain);
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(ct.towers()));
    Allocs out_c0(ct.towers(), ctx.poly_bytes);
    Allocs out_c1(ct.towers(), ctx.poly_bytes);
    REQUIRE(hazeMulMrp(out_c0.data(), ct.c0().as_const().data(), mono_chain.as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulMrp(out_c1.data(), ct.c1().as_const().data(), mono_chain.as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    // Monomial is a literal x^power with coefficient 1 (or -1 for power>=N),
    // NOT a CKKS-encoded plaintext, so NSD/SF/level are preserved.
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg(),
              ct.scaling_factor(), ct.level()};
}

Ct square_ct(const OpCtx &ctx, const Ct &ct) { return mult(ctx, ct, ct); }

// Mirror cc->ModReduceInPlace: real rescale under FIXEDMANUAL, no-op for
// every other scaling technique (LeveledSHERNS::ModReduceInPlace returns
// early when GetScalingTechnique() != FIXEDMANUAL — see
// rns-leveledshe.cpp:335-340). Use this anywhere OpenFHE has
// cc->ModReduceInPlace; use plain rescale() for algo->ModReduceInternal.
Ct manual_rescale(const OpCtx &ctx, Ct ct) {
    if (ctx.mode == lbcrypto::FIXEDMANUAL)
        return rescale(ctx, std::move(ct));
    return ct;
}

// Port of LeveledSHECKKSRNS::AdjustLevelsAndDepthInPlace
// (ckksrns-leveledshe.cpp:603-737). Aligns c1 and c2 to the same level and
// depth (NSD), modifying whichever needs to be brought into line. Composite
// degree always 1 here. Uses both ct's tracked SF (set via Ct's
// scaling_factor field) and the cryptoparameters' level-indexed SF
// (GetScalingFactorReal / GetScalingFactorRealBig / GetModReduceFactor).
//
// Returns the aligned pair. For the c1lvl < c2lvl branch, c1 is brought up
// to c2 (the original `adjust_one_to_other(high, low)` helper had `high`
// as the over-towered one — note that in OpenFHE, c1lvl < c2lvl means
// c1's CRT chain is LONGER (fewer levels dropped), so c1 is the one with
// MORE towers).
[[maybe_unused]] void adjust_levels_and_depth_in_place(const OpCtx &ctx, Ct &c1, Ct &c2) {
    const std::uint32_t c1lvl = c1.level();
    const std::uint32_t c2lvl = c2.level();
    const std::uint32_t c1depth = c1.noise_scale_deg();
    const std::uint32_t c2depth = c2.noise_scale_deg();
    const std::uint32_t sizeQl1 = static_cast<std::uint32_t>(c1.towers());
    const std::uint32_t sizeQl2 = static_cast<std::uint32_t>(c2.towers());
    auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    constexpr std::uint32_t compositeDegree = 1;

    auto adjust_lower = [&](Ct &lower, const Ct &higher,
                            std::uint32_t lowerLvl, std::uint32_t higherLvl,
                            std::uint32_t lowerDepth, std::uint32_t higherDepth,
                            std::uint32_t sizeQlLower) {
        if (lowerDepth == 2) {
            if (higherDepth == 2) {
                const double scf1 = lower.scaling_factor();
                const double scf2 = higher.scaling_factor();
                const double scf = cp->GetScalingFactorReal(lowerLvl);
                const double q1 =
                    cp->GetModReduceFactor(sizeQlLower - 1);
                lower = mult_by_const(ctx, lower, scf2 / scf1 * q1 / scf);
                lower = rescale(ctx, std::move(lower));
                if (lowerLvl + compositeDegree < higherLvl)
                    lower = level_reduce(ctx, std::move(lower),
                                         higherLvl - lowerLvl - compositeDegree);
                lower.set_scaling_factor(higher.scaling_factor());
            } else {
                if (lowerLvl + compositeDegree == higherLvl) {
                    lower = rescale(ctx, std::move(lower));
                } else {
                    const double scf1 = lower.scaling_factor();
                    const double scf2 = cp->GetScalingFactorRealBig(
                        higherLvl - compositeDegree);
                    const double scf = cp->GetScalingFactorReal(lowerLvl);
                    const double q1 =
                        cp->GetModReduceFactor(sizeQlLower - 1);
                    lower = mult_by_const(ctx, lower, scf2 / scf1 * q1 / scf);
                    lower = rescale(ctx, std::move(lower));
                    if (lowerLvl + 2 * compositeDegree < higherLvl)
                        lower = level_reduce(
                            ctx, std::move(lower),
                            higherLvl - lowerLvl - 2 * compositeDegree);
                    lower = rescale(ctx, std::move(lower));
                    lower.set_scaling_factor(higher.scaling_factor());
                }
            }
        } else {
            if (higherDepth == 2) {
                const double scf1 = lower.scaling_factor();
                const double scf2 = higher.scaling_factor();
                const double scf = cp->GetScalingFactorReal(lowerLvl);
                lower = mult_by_const(ctx, lower, scf2 / scf1 / scf);
                lower = level_reduce(ctx, std::move(lower),
                                     higherLvl - lowerLvl);
                lower.set_scaling_factor(scf2);
            } else {
                const double scf1 = lower.scaling_factor();
                const double scf2 = cp->GetScalingFactorRealBig(
                    higherLvl - compositeDegree);
                const double scf = cp->GetScalingFactorReal(lowerLvl);
                lower = mult_by_const(ctx, lower, scf2 / scf1 / scf);
                if (lowerLvl + compositeDegree < higherLvl)
                    lower = level_reduce(ctx, std::move(lower),
                                         higherLvl - lowerLvl - compositeDegree);
                lower = rescale(ctx, std::move(lower));
                lower.set_scaling_factor(higher.scaling_factor());
            }
        }
    };

    if (c1lvl < c2lvl) {
        adjust_lower(c1, c2, c1lvl, c2lvl, c1depth, c2depth, sizeQl1);
    } else if (c1lvl > c2lvl) {
        adjust_lower(c2, c1, c2lvl, c1lvl, c2depth, c1depth, sizeQl2);
    } else {
        if (c1depth < c2depth)
            c1 = mult_by_const(ctx, c1, 1.0);
        else if (c2depth < c1depth)
            c2 = mult_by_const(ctx, c2, 1.0);
    }
}

// Compat shim: brings `high` down to `low`'s level shape. Always takes
// the c1lvl < c2lvl branch of adjust_levels_and_depth_in_place (i.e.
// adjust the higher-tower one down), without needing to mutate `low`.
// Inlined to avoid emitting a clone_ct op that the old implementation
// didn't.
Ct adjust_one_to_other(const OpCtx &ctx, Ct high, const Ct &low) {
    REQUIRE(high.towers() > low.towers());
    auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    constexpr std::uint32_t compositeDegree = 1;
    const std::uint32_t lowerLvl = high.level();
    const std::uint32_t higherLvl = low.level();
    const std::uint32_t lowerDepth = high.noise_scale_deg();
    const std::uint32_t higherDepth = low.noise_scale_deg();
    const std::uint32_t sizeQlLower = static_cast<std::uint32_t>(high.towers());

    if (lowerDepth == 2) {
        if (higherDepth == 2) {
            const double scf1 = high.scaling_factor();
            const double scf2 = low.scaling_factor();
            const double scf = cp->GetScalingFactorReal(lowerLvl);
            const double q1 = cp->GetModReduceFactor(sizeQlLower - 1);
            high = mult_by_const(ctx, high, scf2 / scf1 * q1 / scf);
            high = rescale(ctx, std::move(high));
            if (lowerLvl + compositeDegree < higherLvl)
                high = level_reduce(ctx, std::move(high),
                                    higherLvl - lowerLvl - compositeDegree);
            high.set_scaling_factor(low.scaling_factor());
        } else {
            if (lowerLvl + compositeDegree == higherLvl) {
                high = rescale(ctx, std::move(high));
            } else {
                const double scf1 = high.scaling_factor();
                const double scf2 = cp->GetScalingFactorRealBig(
                    higherLvl - compositeDegree);
                const double scf = cp->GetScalingFactorReal(lowerLvl);
                const double q1 = cp->GetModReduceFactor(sizeQlLower - 1);
                high = mult_by_const(ctx, high, scf2 / scf1 * q1 / scf);
                high = rescale(ctx, std::move(high));
                if (lowerLvl + 2 * compositeDegree < higherLvl)
                    high = level_reduce(
                        ctx, std::move(high),
                        higherLvl - lowerLvl - 2 * compositeDegree);
                high = rescale(ctx, std::move(high));
                high.set_scaling_factor(low.scaling_factor());
            }
        }
    } else {
        if (higherDepth == 2) {
            const double scf1 = high.scaling_factor();
            const double scf2 = low.scaling_factor();
            const double scf = cp->GetScalingFactorReal(lowerLvl);
            high = mult_by_const(ctx, high, scf2 / scf1 / scf);
            high = level_reduce(ctx, std::move(high),
                                higherLvl - lowerLvl);
            high.set_scaling_factor(scf2);
        } else {
            const double scf1 = high.scaling_factor();
            const double scf2 =
                cp->GetScalingFactorRealBig(higherLvl - compositeDegree);
            const double scf = cp->GetScalingFactorReal(lowerLvl);
            high = mult_by_const(ctx, high, scf2 / scf1 / scf);
            if (lowerLvl + compositeDegree < higherLvl)
                high = level_reduce(ctx, std::move(high),
                                    higherLvl - lowerLvl - compositeDegree);
            high = rescale(ctx, std::move(high));
            high.set_scaling_factor(low.scaling_factor());
        }
    }
    return high;
}

// Mirror OpenFHE's LeveledSHECKKSRNS::AdjustLevelsAndDepthToOneInPlace
// (ckksrns-leveledshe.cpp:736). Equalize levels via adjust_one_to_other,
// equalize depths via mult_by_const(_, 1.0), then if both NSD=2 ModReduce
// both. Output: a and b at the same level, both at NSD=1.
AdjustedPair adjust_for_mult(const OpCtx &ctx, Ct a, Ct b) {
    if (ctx.mode == lbcrypto::FIXEDMANUAL) {
        // FIXEDMANUAL: AdjustLevelsInPlace — drop towers from the higher
        // one to match the lower one. No scale-factor adjustment.
        if (a.towers() > b.towers())
            a = level_reduce(ctx, std::move(a), a.towers() - b.towers());
        else if (b.towers() > a.towers())
            b = level_reduce(ctx, std::move(b), b.towers() - a.towers());
        return {std::move(a), std::move(b)};
    }
    if (a.towers() > b.towers())
        a = adjust_one_to_other(ctx, std::move(a), b);
    else if (b.towers() > a.towers())
        b = adjust_one_to_other(ctx, std::move(b), a);

    REQUIRE(a.towers() == b.towers());

    if (a.noise_scale_deg() < b.noise_scale_deg())
        a = mult_by_const(ctx, a, 1.0);
    else if (b.noise_scale_deg() < a.noise_scale_deg())
        b = mult_by_const(ctx, b, 1.0);

    if (a.noise_scale_deg() == 2 && b.noise_scale_deg() == 2) {
        a = rescale(ctx, std::move(a));
        b = rescale(ctx, std::move(b));
    }
    return {std::move(a), std::move(b)};
}

// EvalSquare's adjust: AdjustLevelsAndDepthToOneInPlace on a single
// ciphertext (OpenFHE runs it with both args aliasing the same ct). With
// equal towers/depth the level and depth alignment is a no-op, so this
// reduces to "rescale once if NSD==2" — exactly what adjust_for_mult(x, x)
// computes for its .a, but without the duplicate .b rescale that the square
// callers discard. Returns the single adjusted operand to square.
Ct adjust_for_square(const OpCtx &ctx, Ct x) {
    if (ctx.mode != lbcrypto::FIXEDMANUAL && x.noise_scale_deg() == 2)
        return rescale(ctx, std::move(x));
    return x;
}

// Mirror OpenFHE's LeveledSHECKKSRNS::AdjustLevelsAndDepthInPlace
// (ckksrns-leveledshe.cpp:603) — same level alignment as the mult
// variant, just without the To-One ModReduce-both wrap. Used by EvalAdd
// and EvalSub.
AdjustedPair adjust_for_add(const OpCtx &ctx, Ct a, Ct b) {
    if (ctx.mode == lbcrypto::FIXEDMANUAL) {
        // FIXEDMANUAL: AdjustForAddOrSubInPlace calls AdjustLevelsInPlace,
        // then optionally encodes a powP plaintext for depth mismatch — but
        // the depth-mismatch branch only fires when one operand is a
        // plaintext, not the ct-ct case. So just level-align.
        if (a.towers() > b.towers())
            a = level_reduce(ctx, std::move(a), a.towers() - b.towers());
        else if (b.towers() > a.towers())
            b = level_reduce(ctx, std::move(b), b.towers() - a.towers());
        return {std::move(a), std::move(b)};
    }
    if (a.towers() > b.towers())
        a = adjust_one_to_other(ctx, std::move(a), b);
    else if (b.towers() > a.towers())
        b = adjust_one_to_other(ctx, std::move(b), a);

    REQUIRE(a.towers() == b.towers());

    if (a.noise_scale_deg() < b.noise_scale_deg())
        a = mult_by_const(ctx, a, 1.0);
    else if (b.noise_scale_deg() < a.noise_scale_deg())
        b = mult_by_const(ctx, b, 1.0);
    return {std::move(a), std::move(b)};
}

// Mirrors OpenFHE's Degree() (ckksrns-utils.h:88-98), which defaults to
// `delta = 0.0` — STRICT-zero comparison, not the 0x1p-44 epsilon used
// elsewhere by IsNotEqualZero/IsNotEqualOne. Coefficients like
// 8.17e-15 in g_coefficientsUniform are below 0x1p-44 (so IsNotEqualZero
// would treat them as zero), but OpenFHE's Degree() still counts them as
// non-zero — which means OpenFHE's f2 retains them after `f2.resize(Degree+1)`
// while a 0x1p-44 threshold would drop them. Phase 25 caught this by
// adding idx 85 = 8.17e-15 and finding 2048 mismatches.
uint32_t poly_degree(const std::vector<double> &v) {
    for (std::size_t i = v.size(); i-- > 0;)
        if (v[i] != 0.0)
            return static_cast<std::uint32_t>(i);
    return 0;
}

// Lazily-cached, shared rescaled view of the Chebyshev power tree. Every
// consumer of T[i] in the PS evaluation that auto-rescales an NSD=2 power
// before mult_by_const (eval_mult_scalar in the baby-step accumulators,
// EvalPartialLinearWSum, the cu term) reads operand(i) here instead. rescale
// is a pure function of T[i], so one rescale per power is shared across every
// sweep, every EvalPartialLinearWSum term, and the whole recursion —
// value-identical, but it elides the duplicate INTT/ModDown/NTT round-trips.
// operand(i) returns the raw T[i] when no rescale applies (FIXEDMANUAL, or
// NSD!=2), matching eval_mult_scalar's own branch.
struct RescaledTree {
    const OpCtx &ctx;
    const std::vector<Ct> &T;
    bool active;
    mutable std::vector<std::optional<Ct>> cache;
    RescaledTree(const OpCtx &c, const std::vector<Ct> &t)
        : ctx(c), T(t), active(c.mode != lbcrypto::FIXEDMANUAL), cache(t.size()) {}
    const Ct &operand(std::size_t i) const {
        if (active && T[i].noise_scale_deg() == 2) {
            if (!cache[i].has_value())
                cache[i] = rescale(ctx, T[i]);
            return *cache[i];
        }
        return T[i];
    }
};

// EvalPartialLinearWSum: Σ T[i] * coeffs[i+1] for i in 0..n-1. Mirrors
// OpenFHE's EvalPartialLinearWSum (advancedshe.cpp:143). Important byte
// invariant: OpenFHE multiplies by EVERY coefficient including ones below
// the IsNotEqualZero threshold — must not skip tiny coefs or the IR
// sequence diverges from OpenFHE's. When `rt` is supplied, the (possibly
// rescaled) operands come from the shared tree so each power is rescaled at
// most once across the entire PS evaluation.
Ct eval_partial_linear_wsum(const OpCtx &ctx, const std::vector<Ct> &T,
                            const std::vector<double> &coeffs, std::uint32_t n,
                            const RescaledTree *rt = nullptr) {
    REQUIRE(n > 0);
    if (rt != nullptr) {
        Ct acc = mult_by_const(ctx, rt->operand(0), coeffs[1]);
        for (std::uint32_t i = 1; i < n; ++i) {
            Ct term = mult_by_const(ctx, rt->operand(i), coeffs[i + 1]);
            acc = add(ctx, acc, term);
        }
        return manual_rescale(ctx, std::move(acc));
    }
    std::vector<Ct> cts;
    cts.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
        cts.push_back(clone_ct(ctx, T[i]));
    if (ctx.mode != lbcrypto::FIXEDMANUAL && cts.front().noise_scale_deg() == 2) {
        for (auto &c : cts)
            c = rescale(ctx, std::move(c));
    }

    Ct acc = mult_by_const(ctx, cts[0], coeffs[1]);
    for (std::uint32_t i = 1; i < n; ++i) {
        Ct term = mult_by_const(ctx, cts[i], coeffs[i + 1]);
        acc = add(ctx, acc, term);
    }
    // OpenFHE EvalPartialLinearWSum ends with cc->ModReduceInPlace(acc) —
    // real rescale under FIXEDMANUAL, no-op otherwise.
    return manual_rescale(ctx, std::move(acc));
}

// Compute the Chebyshev power tree T[0..k-1] and T2[0..m-1] from input ct.
// Mirrors internalEvalChebyPolysPS (OpenFHE ckksrns-advancedshe.cpp:967).
struct ChebyTree {
    std::vector<Ct> T;
    std::vector<Ct> T2;
    Ct T2km1;
    std::uint32_t k;
    std::uint32_t m;
};

ChebyTree compute_cheby_tree(const OpCtx &ctx, const Ct &x, std::uint32_t k, std::uint32_t m) {
    std::vector<Ct> T;
    T.reserve(k);
    // T[0] = x (a=-1, b=1 — no linear transformation).
    T.push_back(clone_ct(ctx, x));
    // FIXEDMANUAL needs explicit rescale per iteration; FIXEDAUTO defers
    // rescaling via auto-rescale on the next EvalMult/Square with NSD>1
    // input. Mirror both code paths.
    const bool fixed_manual = (ctx.mode == lbcrypto::FIXEDMANUAL);
    auto maybe_rescale = [&](Ct ct) -> Ct {
        return fixed_manual ? rescale(ctx, std::move(ct)) : std::move(ct);
    };
    // pre_rescale was needed before adjust_for_mult existed; now obsolete.
    for (std::uint32_t i = 2; i <= k; ++i) {
        if (i & 0x1) {
            // T[2j+1] = 2*T[j]*T[j+1] - x. Mirrors FIDESlib's pattern at
            // ApproxModEval.cu:478-485 — adjustForMult on both operands,
            // then mult, then adjustForAddOrSub before sub with T[0].
            auto mp = adjust_for_mult(ctx, clone_ct(ctx, T[i / 2 - 1]),
                                     clone_ct(ctx, T[i / 2]));
            Ct prod = mult(ctx, mp.a, mp.b);
            Ct doubled = mult_int_scalar(ctx, prod, 2);
            Ct rescaled = maybe_rescale(std::move(doubled));
            auto s = adjust_for_add(ctx, std::move(rescaled), clone_ct(ctx, T[0]));
            T.push_back(sub(ctx, s.a, s.b));
        } else {
            // T[2j] = 2*T[j]² - 1. EvalSquare adjusts the single operand once.
            Ct adj = adjust_for_square(ctx, clone_ct(ctx, T[i / 2 - 1]));
            Ct sq = square_ct(ctx, adj);
            Ct doubled = mult_int_scalar(ctx, sq, 2);
            Ct rescaled = maybe_rescale(std::move(doubled));
            T.push_back(add_const(ctx, rescaled, -1.0));
        }
    }
    // Mirror OpenFHE: bring each T[i] to T[k-1]'s level/depth.
    // FIXEDMANUAL: plain LevelReduceInPlace (ckksrns-advancedshe.cpp:1013-1016).
    // Other modes: AdjustLevelsAndDepthInPlace via adjust_one_to_other.
    if (ctx.mode == lbcrypto::FIXEDMANUAL) {
        for (std::uint32_t i = 0; i + 1 < k; ++i) {
            if (T[i].towers() > T.back().towers())
                T[i] = level_reduce(ctx, std::move(T[i]),
                                    T[i].towers() - T.back().towers());
        }
    } else {
        for (std::uint32_t i = 0; i + 1 < k; ++i) {
            if (T[i].towers() > T.back().towers())
                T[i] = adjust_one_to_other(ctx, std::move(T[i]), T.back());
            if (T[i].noise_scale_deg() < T.back().noise_scale_deg())
                T[i] = mult_by_const(ctx, T[i], 1.0);
            // The shared rescaled-tree hoist (RescaledTree) gates rescale on a
            // per-power NSD check; that matches OpenFHE's per-front-NSD gate in
            // EvalPartialLinearWSum only because every power lands at the same
            // NSD here. Enforce that invariant so a future parameter regime
            // that breaks it fails loudly instead of diverging silently.
            REQUIRE(T[i].noise_scale_deg() == T.back().noise_scale_deg());
        }
    }

    std::vector<Ct> T2;
    T2.reserve(m);
    T2.push_back(clone_ct(ctx, T.back()));
    Ct T2km1 = clone_ct(ctx, T.back());
    for (std::uint32_t i = 1; i < m; ++i) {
        Ct sq_adj = adjust_for_square(ctx, clone_ct(ctx, T2[i - 1]));
        Ct sq = square_ct(ctx, sq_adj);
        Ct doubled = mult_int_scalar(ctx, sq, 2);
        Ct rescaled = maybe_rescale(std::move(doubled));
        T2.push_back(add_const(ctx, rescaled, -1.0));

        // T2km1 = 2*T2km1*T2[i] - T2[0]
        auto mp = adjust_for_mult(ctx, std::move(T2km1), clone_ct(ctx, T2[i]));
        Ct prod = mult(ctx, mp.a, mp.b);
        Ct dbl = mult_int_scalar(ctx, prod, 2);
        Ct rs = maybe_rescale(std::move(dbl));
        auto sp = adjust_for_add(ctx, std::move(rs), clone_ct(ctx, T2[0]));
        T2km1 = sub(ctx, sp.a, sp.b);
    }
    return ChebyTree{std::move(T), std::move(T2), std::move(T2km1), k, m};
}

// Recursive Paterson-Stockmeyer Chebyshev series evaluation. Mirrors
// the UPSTREAM (non-fused) OpenFHE InnerEvalChebyshevPS at
// ckksrns-advancedshe.cpp:849 — separate passes for qu / su / cu, each
// loading T[0..k-1] independently.
//
// Note: OpenFHE built with WITH_CPROBES=ON (our case) dispatches
// EvalChebyshevSeries to the niobium-fused InnerEvalChebyshevPS_NB
// instead (advancedshe.cpp:666), which uses one shared sweep across
// the baby steps and per-accumulator ModReduces. This upstream
// variant produces correct slot-level output (phase 3 / 4 pass) but
// diverges byte-for-byte from cc->EvalChebyshevSeries with CPROBES on.
// inner_eval_chebyshev_ps_nb below mirrors the NB variant for parity.
Ct inner_eval_chebyshev_ps(const OpCtx &ctx, const Ct &x, const std::vector<double> &coefficients,
                           std::uint32_t k, std::uint32_t m, const std::vector<Ct> &T,
                           const std::vector<Ct> &T2) {
    using namespace lbcrypto;
    const std::uint32_t k2m2k = k * (1u << (m - 1)) - k;

    std::vector<double> Tkm(k2m2k + k + 1, 0.0);
    Tkm.back() = 1.0;
    auto divqr = LongDivisionChebyshev(coefficients, Tkm);

    auto &r2 = divqr->r;
    if (std::uint32_t n = poly_degree(r2); static_cast<std::int32_t>(k2m2k - n) <= 0) {
        r2.resize(n + 1);
        r2[k2m2k] -= 1.0;
    } else {
        r2.resize(k2m2k + 1, 0.0);
        r2.back() = -1.0;
    }
    auto divcs = LongDivisionChebyshev(r2, divqr->q);

    auto &s2 = divcs->r;
    s2.resize(k2m2k + 1, 0.0);
    s2.back() = 1.0;

    // Compute qu via lambda — no placeholder alloc, branch returns directly.
    Ct qu = [&]() -> Ct {
        if (poly_degree(divqr->q) > k)
            return inner_eval_chebyshev_ps(ctx, x, divqr->q, k, m - 1, T, T2);
        Ct q = clone_ct(ctx, T[k - 1]);
        const std::uint32_t limit =
            static_cast<std::uint32_t>(std::log2(std::abs(divqr->q.back())));
        for (std::uint32_t i = 0; i < limit; ++i)
            q = mult_int_scalar(ctx, q, 2);
        q = add_const(ctx, q, divqr->q.front() / 2.0);

        divqr->q.resize(k);
        if (std::uint32_t n = poly_degree(divqr->q); n > 0) {
            Ct extra = eval_partial_linear_wsum(ctx, T, divqr->q, n);
            Ct extra_r = rescale(ctx, extra);
            // Align by metadata only — OpenFHE LevelReduceInPlace path.
            if (extra_r.towers() > q.towers())
                extra_r = level_reduce(ctx, std::move(extra_r), extra_r.towers() - q.towers());
            else if (q.towers() > extra_r.towers())
                q = level_reduce(ctx, std::move(q), q.towers() - extra_r.towers());
            q = add(ctx, q, extra_r);
        }
        return q;
    }();

    // Compute su.
    Ct su = [&]() -> Ct {
        if (poly_degree(s2) > k)
            return inner_eval_chebyshev_ps(ctx, x, s2, k, m - 1, T, T2);
        Ct s = clone_ct(ctx, T[k - 1]);
        s2.resize(k);
        if (std::uint32_t n = poly_degree(s2); n > 0) {
            Ct extra = eval_partial_linear_wsum(ctx, T, s2, n);
            Ct extra_r = rescale(ctx, extra);
            if (extra_r.towers() > s.towers())
                extra_r = level_reduce(ctx, std::move(extra_r), extra_r.towers() - s.towers());
            else if (s.towers() > extra_r.towers())
                s = level_reduce(ctx, std::move(s), s.towers() - extra_r.towers());
            s = add(ctx, s, extra_r);
        }
        s = add_const(ctx, s, s2.front() / 2.0);
        // Reference: cc->LevelReduceInPlace(su, nullptr) — metadata-only.
        if (s.towers() >= 2)
            s = level_reduce(ctx, std::move(s), 1);
        return s;
    }();

    // Compute cu (may be empty if no non-zero divcs->q coefficient).
    std::optional<Ct> cu;
    if (std::uint32_t n = poly_degree(divcs->q); n >= 1) {
        Ct c = [&]() -> Ct {
            if (n == 1) {
                if (std::abs(divcs->q[1] - 1.0) > 0x1p-44)
                    return rescale(ctx, mult_by_const(ctx, T[0], divcs->q[1]));
                return clone_ct(ctx, T[0]);
            }
            return rescale(ctx, eval_partial_linear_wsum(ctx, T, divcs->q, n));
        }();
        c = add_const(ctx, c, divcs->q.front() / 2.0);
        // Reference: LevelReduceInPlace(cu, nullptr, ...) — metadata-only.
        if (c.towers() > T2[m - 1].towers())
            c = level_reduce(ctx, std::move(c), c.towers() - T2[m - 1].towers());
        cu = std::move(c);
    }

    Ct cu_t2 = [&]() -> Ct {
        if (cu.has_value()) {
            // Align T2[m-1] tower count to cu via metadata-only drop.
            Ct t2m1 = clone_ct(ctx, T2[m - 1]);
            if (t2m1.towers() > cu->towers())
                t2m1 = level_reduce(ctx, std::move(t2m1), t2m1.towers() - cu->towers());
            else if (cu->towers() > t2m1.towers())
                *cu = level_reduce(ctx, std::move(*cu), cu->towers() - t2m1.towers());
            return add(ctx, *cu, t2m1);
        }
        return add_const(ctx, clone_ct(ctx, T2[m - 1]), divcs->q.front() / 2.0);
    }();

    // result = cu_t2 * qu + su. Mirrors OpenFHE's:
    //   auto result = cc->EvalMult(cu, qu);
    //   cc->ModReduceInPlace(result);  // no-op in FIXEDAUTO
    //   cc->EvalAddInPlace(result, su);
    // EvalMult in FIXEDAUTO calls AdjustForMult which rescales both to
    // NSD=1 first, so the mult output is NSD=2 (not NSD=4 from raw mult).
    auto rp = adjust_for_mult(ctx, std::move(cu_t2), std::move(qu));
    Ct result = mult(ctx, rp.a, rp.b);
    // FIXEDAUTO ModReduceInPlace is a no-op; only rescale in FIXEDMANUAL.
    if (ctx.mode == lbcrypto::FIXEDMANUAL)
        result = rescale(ctx, result);
    // EvalAddInPlace adjusts levels — mirror via adjust_for_add.
    auto ap = adjust_for_add(ctx, std::move(result), std::move(su));
    return add(ctx, ap.a, ap.b);
}

// Mirror of niobium::AccumBabyStep (advancedshe.cpp:654): if scalar is
// non-zero, mult `prepared` by it and accumulate into `acc`. `prepared` is
// the eval_mult_scalar-equivalent operand (already rescaled when the source
// T[i] was at NSD=2) — see the fused base case, which rescales each T[i]
// once and reuses it across the acc_q / acc_s / acc_c sweeps.
void accum_baby_step(const OpCtx &ctx, std::optional<Ct> &acc, const Ct &prepared, double scalar) {
    if (std::abs(scalar) <= 0x1p-44)
        return;
    Ct tmp = mult_by_const(ctx, prepared, scalar);
    if (acc.has_value())
        *acc = add(ctx, *acc, tmp);
    else
        acc.emplace(std::move(tmp));
}

// Niobium-fused PS Chebyshev recursion. Byte-exact mirror of
// niobium::InnerEvalChebyshevPS_NB (ckksrns-advancedshe.cpp:666). The
// fused base case (q_base && s_base) does ONE sweep over T[0..k-1]
// accumulating into acc_q / acc_s / acc_c simultaneously, then
// ModReduces each accumulator independently — this is the path
// cc->EvalChebyshevSeries dispatches to under WITH_CPROBES=ON, and
// the source of the 4-tower-budget gap that the upstream variant
// above did not match.
//
// The non-fused (recursive) branch matches OpenFHE's
// ORIGINAL PATH (advancedshe.cpp:783-836). Same primitives as the
// upstream variant but with EvalPartialLinearWSum-style ordering: the
// final EvalAddInPlace(qu, EvalPartialLinearWSum) happens AFTER
// add_const(qu, q_free/2), and su's add_const happens AFTER the
// partial-linear-wsum (opposite of the upstream variant's order).
Ct inner_eval_chebyshev_ps_nb(const OpCtx &ctx, const Ct &x,
                              const std::vector<double> &coefficients,
                              std::uint32_t k, std::uint32_t m,
                              const std::vector<Ct> &T, const std::vector<Ct> &T2,
                              const RescaledTree &rt) {
    using namespace lbcrypto;
    const std::uint32_t k2m2k = k * (1u << (m - 1)) - k;

    std::vector<double> Tkm(k2m2k + k + 1, 0.0);
    Tkm.back() = 1.0;
    auto divqr = LongDivisionChebyshev(coefficients, Tkm);

    auto &r2 = divqr->r;
    if (std::uint32_t n = poly_degree(r2); static_cast<std::int32_t>(k2m2k - n) <= 0) {
        r2.resize(n + 1);
        r2[k2m2k] -= 1.0;
    } else {
        r2.resize(k2m2k + 1, 0.0);
        r2.back() = -1.0;
    }
    auto divcs = LongDivisionChebyshev(r2, divqr->q);

    auto &s2 = divcs->r;
    s2.resize(k2m2k + 1, 0.0);
    s2.back() = 1.0;

    const bool q_base = (poly_degree(divqr->q) <= k);
    const bool s_base = (poly_degree(s2) <= k);

    std::optional<Ct> cu;
    Ct qu = clone_ct(ctx, T[k - 1]); // placeholder; reassigned below
    Ct su = clone_ct(ctx, T[k - 1]);

    if (q_base && s_base) {
        // Fused base case.
        qu = clone_ct(ctx, T[k - 1]);
        const std::uint32_t q_lead =
            static_cast<std::uint32_t>(std::log2(std::abs(divqr->q.back())));
        for (std::uint32_t i = 0; i < q_lead; ++i)
            qu = mult_int_scalar(ctx, qu, 2);

        su = clone_ct(ctx, T[k - 1]);

        const double q_free = divqr->q.front();
        divqr->q.resize(k);
        const std::uint32_t nq = poly_degree(divqr->q);

        const double s_free = s2.front();
        s2.resize(k);
        const std::uint32_t ns = poly_degree(s2);

        const std::uint32_t nc = poly_degree(divcs->q);

        const std::uint32_t max_deg = std::max({nq, ns, (nc > 1) ? nc : 0u});
        std::optional<Ct> acc_q, acc_s, acc_c;
        // The same baby-step power feeds up to three accumulators; rt.operand
        // hands back its shared (once-)rescaled form, so the duplicate
        // INTT/ModDown/NTT round-trips never get emitted.
        for (std::uint32_t i = 0; i < max_deg; ++i) {
            if (i < nq && std::abs(divqr->q[i + 1]) > 0x1p-44)
                accum_baby_step(ctx, acc_q, rt.operand(i), divqr->q[i + 1]);
            if (i < ns && std::abs(s2[i + 1]) > 0x1p-44)
                accum_baby_step(ctx, acc_s, rt.operand(i), s2[i + 1]);
            if (nc > 1 && i < nc && std::abs(divcs->q[i + 1]) > 0x1p-44)
                accum_baby_step(ctx, acc_c, rt.operand(i), divcs->q[i + 1]);
        }

        // OpenFHE: `cc->ModReduceInPlace(acc_q); cc->EvalAddInPlace(qu, acc_q);`
        // For non-FIXEDMANUAL, ModReduceInPlace is a no-op and the implicit
        // level alignment in EvalAddInPlace handles things. For FIXEDMANUAL
        // it's a real rescale that must fire here.
        if (acc_q.has_value()) {
            Ct r = manual_rescale(ctx, std::move(*acc_q));
            auto ap = adjust_for_add(ctx, std::move(qu), std::move(r));
            qu = add(ctx, ap.a, ap.b);
        }
        if (acc_s.has_value()) {
            Ct r = manual_rescale(ctx, std::move(*acc_s));
            auto ap = adjust_for_add(ctx, std::move(su), std::move(r));
            su = add(ctx, ap.a, ap.b);
        }

        qu = add_const(ctx, qu, q_free / 2.0);
        su = add_const(ctx, su, s_free / 2.0);
        // OpenFHE: cc->LevelReduceInPlace(su, nullptr). For FIXEDAUTO this
        // is a no-op (LeveledSHERNS::LevelReduceInPlace only does work for
        // FIXEDMANUAL — see rns-leveledshe.cpp:359). Mirror that.
        if (ctx.mode == lbcrypto::FIXEDMANUAL && su.towers() >= 2)
            su = level_reduce(ctx, std::move(su), 1);

        // OpenFHE: `cu = cc->EvalMult(T.front(), divcs->q[1]); ModReduceInPlace(cu)`.
        // cc->EvalMult is the auto-rescale wrapper (eval_mult_scalar) which
        // is a no-op rescale-wise in FIXEDMANUAL; the trailing ModReduceInPlace
        // is the FIXEDMANUAL rescale. For the nc>1 branch OpenFHE does
        // `ModReduceInPlace(acc_c); cu = acc_c` — again, FIXEDMANUAL only.
        if (nc == 1) {
            if (std::abs(divcs->q[1] - 1.0) > 0x1p-44) {
                Ct r = mult_by_const(ctx, rt.operand(0), divcs->q[1]);
                cu = manual_rescale(ctx, std::move(r));
            } else {
                cu = clone_ct(ctx, T[0]);
            }
        } else if (acc_c.has_value()) {
            cu = manual_rescale(ctx, std::move(*acc_c));
        }

        if (cu.has_value()) {
            *cu = add_const(ctx, *cu, divcs->q.front() / 2.0);
            // OpenFHE: LevelReduceInPlace(cu, nullptr, ...). FIXEDAUTO no-op.
            if (ctx.mode == lbcrypto::FIXEDMANUAL && cu->towers() > T2[m - 1].towers())
                *cu = level_reduce(ctx, std::move(*cu), cu->towers() - T2[m - 1].towers());
        }
    } else {
        // Original recursive path (mirrors OpenFHE 783-836).
        if (poly_degree(divqr->q) > k) {
            qu = inner_eval_chebyshev_ps_nb(ctx, x, divqr->q, k, m - 1, T, T2, rt);
        } else {
            qu = clone_ct(ctx, T[k - 1]);
            const std::uint32_t limit =
                static_cast<std::uint32_t>(std::log2(std::abs(divqr->q.back())));
            for (std::uint32_t i = 0; i < limit; ++i)
                qu = mult_int_scalar(ctx, qu, 2);
            qu = add_const(ctx, qu, divqr->q.front() / 2.0);

            divqr->q.resize(k);
            if (std::uint32_t n = poly_degree(divqr->q); n > 0) {
                // OpenFHE: EvalAddInPlace(qu, EvalPartialLinearWSum(...)).
                // eval_partial_linear_wsum already does the upfront ModReduce
                // and the trailing rescale is a no-op in FIXEDAUTO; let
                // adjust_for_add handle any final level alignment.
                Ct extra = eval_partial_linear_wsum(ctx, T, divqr->q, n, &rt);
                auto ap = adjust_for_add(ctx, std::move(qu), std::move(extra));
                qu = add(ctx, ap.a, ap.b);
            }
        }

        if (poly_degree(s2) > k) {
            su = inner_eval_chebyshev_ps_nb(ctx, x, s2, k, m - 1, T, T2, rt);
        } else {
            su = clone_ct(ctx, T[k - 1]);
            s2.resize(k);
            if (std::uint32_t n = poly_degree(s2); n > 0) {
                Ct extra = eval_partial_linear_wsum(ctx, T, s2, n, &rt);
                auto ap = adjust_for_add(ctx, std::move(su), std::move(extra));
                su = add(ctx, ap.a, ap.b);
            }
            su = add_const(ctx, su, s2.front() / 2.0);
            if (ctx.mode == lbcrypto::FIXEDMANUAL && su.towers() >= 2)
                su = level_reduce(ctx, std::move(su), 1);
        }

        if (std::uint32_t n = poly_degree(divcs->q); n >= 1) {
            Ct c = [&]() -> Ct {
                if (n == 1) {
                    // OpenFHE: cu = cc->EvalMult(T[0], divcs->q[1]); ModReduceInPlace(cu);
                    if (std::abs(divcs->q[1] - 1.0) > 0x1p-44) {
                        Ct r = mult_by_const(ctx, rt.operand(0), divcs->q[1]);
                        return manual_rescale(ctx, std::move(r));
                    }
                    return clone_ct(ctx, T[0]);
                }
                // eval_partial_linear_wsum already trails manual_rescale.
                return eval_partial_linear_wsum(ctx, T, divcs->q, n, &rt);
            }();
            c = add_const(ctx, c, divcs->q.front() / 2.0);
            if (ctx.mode == lbcrypto::FIXEDMANUAL && c.towers() > T2[m - 1].towers())
                c = level_reduce(ctx, std::move(c), c.towers() - T2[m - 1].towers());
            cu = std::move(c);
        }
    }

    // Common tail (advancedshe.cpp:838-843):
    //   cu = cu ? EvalAdd(T2[m-1], cu) : EvalAdd(T2[m-1], divcs->q.front()/2);
    //   result = EvalMult(cu, qu); ModReduce; EvalAddInPlace(result, su);
    Ct cu_t2 = [&]() -> Ct {
        if (cu.has_value()) {
            auto ap = adjust_for_add(ctx, clone_ct(ctx, T2[m - 1]), std::move(*cu));
            return add(ctx, ap.a, ap.b);
        }
        return add_const(ctx, clone_ct(ctx, T2[m - 1]), divcs->q.front() / 2.0);
    }();

    auto rp = adjust_for_mult(ctx, std::move(cu_t2), std::move(qu));
    Ct result = mult(ctx, rp.a, rp.b);
    if (ctx.mode == lbcrypto::FIXEDMANUAL)
        result = rescale(ctx, result);
    auto ap = adjust_for_add(ctx, std::move(result), std::move(su));
    return add(ctx, ap.a, ap.b);
}

// Top-level Chebyshev series evaluation.
Ct eval_chebyshev_series(const OpCtx &ctx, const Ct &x,
                         const std::vector<double> &coefficients) {
    const std::uint32_t n = poly_degree(coefficients);
    std::vector<double> f2 = coefficients;
    f2.resize(n + 1);

    auto degs = lbcrypto::ComputeDegreesPS(n);
    const std::uint32_t k = degs[0];
    const std::uint32_t m = degs[1];

    const std::uint32_t k2m2k = k * (1u << (m - 1)) - k;
    // Match OpenFHE internalEvalChebyshevSeriesPSWithPrecomp: resize and
    // unconditionally set last coefficient to 1.
    f2.resize(poly_degree(f2) + 1);
    f2.resize(2 * k2m2k + k + 1, 0.0);
    f2.back() = 1.0;

    ChebyTree tree = compute_cheby_tree(ctx, x, k, m);
    // Dispatch to the NB-fused variant — that's what cc->EvalChebyshevSeries
    // resolves to under WITH_CPROBES=ON. The upstream non-fused variant
    // (inner_eval_chebyshev_ps) remains in this file for reference.
    // The shared rescaled-tree view rescales each baby-step power at most once
    // across the entire (recursive) PS evaluation.
    RescaledTree rt(ctx, tree.T);
    Ct inner = inner_eval_chebyshev_ps_nb(ctx, x, f2, k, m, tree.T, tree.T2, rt);

    // result = inner - T2km1, mirroring cc->EvalSub which routes through
    // AdjustForAddOrSubInPlace = AdjustLevelsAndDepthInPlace for FIXEDAUTO.
    // The previous metadata-only drop_levels skipped that arithmetic and
    // produced byte-divergent output (phase 14 caught the global mismatch).
    auto ap = adjust_for_add(ctx, std::move(inner), clone_ct(ctx, tree.T2km1));
    return sub(ctx, ap.a, ap.b);
}

void apply_double_angle_iterations(const OpCtx &ctx, Ct &ct, std::uint32_t num_iter) {
    constexpr double twoPi = 2.0 * M_PI;
    for (std::int32_t i = 1 - static_cast<std::int32_t>(num_iter); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        // OpenFHE's EvalSquareInPlace runs AdjustLevelsAndDepthToOneInPlace
        // on its single input pair first — drops the level when NSD=2 — so
        // we must mirror that here. My ops::mult doesn't auto-adjust, so do
        // it explicitly before squaring.
        Ct sq_in = adjust_for_square(ctx, clone_ct(ctx, ct));
        ct = square_ct(ctx, sq_in);
        // 2*ct + scalar — avoid add(ct, ct) self-add (trace-output replay
        // divergence).
        Ct doubled = mult_int_scalar(ctx, ct, 2);
        ct = add_const(ctx, doubled, scalar);
        // OpenFHE trails each iter with cc->ModReduceInPlace (real for
        // FIXEDMANUAL, no-op otherwise).
        ct = manual_rescale(ctx, std::move(ct));
    }
}

} // namespace

// Public wrappers for per-phase unit-tests.
Ct add_const_for_test(const OpCtx &ctx, const Ct &ct, double s) { return add_const(ctx, ct, s); }
Ct mult_by_const_for_test(const OpCtx &ctx, const Ct &ct, double s) {
    return mult_by_const(ctx, ct, s);
}
Ct mult_int_scalar_for_test(const OpCtx &ctx, const Ct &ct, std::uint64_t s) {
    return mult_int_scalar(ctx, ct, s);
}
Ct mult_monomial_for_test(const OpCtx &ctx, const Ct &ct, std::uint32_t p) {
    return mult_monomial(ctx, ct, p);
}
Ct square_ct_for_test(const OpCtx &ctx, const Ct &ct) { return square_ct(ctx, ct); }
void apply_double_angle_for_test(const OpCtx &ctx, Ct &ct, std::uint32_t r) {
    apply_double_angle_iterations(ctx, ct, r);
}
Ct eval_chebyshev_series_for_test(const OpCtx &ctx, const Ct &x,
                                  const std::vector<double> &c) {
    return eval_chebyshev_series(ctx, x, c);
}
Ct eval_mod_for_test(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct) {
    return eval_mod(ctx, bk, ct);
}
AdjustedPair adjust_for_mult_for_test(const OpCtx &ctx, Ct a, Ct b) {
    return adjust_for_mult(ctx, std::move(a), std::move(b));
}
AdjustedPair adjust_for_add_for_test(const OpCtx &ctx, Ct a, Ct b) {
    return adjust_for_add(ctx, std::move(a), std::move(b));
}
Ct eval_mult_scalar_for_test(const OpCtx &ctx, const Ct &ct, double scalar) {
    return eval_mult_scalar(ctx, ct, scalar);
}
ChebyTreeForTest compute_cheby_tree_for_test(const OpCtx &ctx, const Ct &x,
                                              const std::vector<double> &coeffs) {
    const std::uint32_t n = poly_degree(coeffs);
    auto degs = lbcrypto::ComputeDegreesPS(n);
    const std::uint32_t k = degs[0];
    const std::uint32_t m = degs[1];
    ChebyTree tree = compute_cheby_tree(ctx, x, k, m);
    return ChebyTreeForTest{std::move(tree.T), std::move(tree.T2), std::move(tree.T2km1), k, m};
}

Ct eval_mod(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct) {
    using namespace lbcrypto;
    const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
    const bool fully_packed = (bk.params.slots == N / 2);

    // g_coefficientsUniform for UNIFORM_TERNARY (FHECKKSRNS header).
    static const std::vector<double> coefficients{
        0.15421426400235561,    -0.0037671538417132409,  0.16032011744533031,
        -0.0034539657223742453, 0.17711481926851286,     -0.0027619720033372291,
        0.19949802549604084,    -0.0015928034845171929,  0.21756948616367638,
        0.00010729951647566607, 0.21600427371240055,     0.0022171399198851363,
        0.17647500259573556,    0.0042856217194480991,   0.086174491919472254,
        0.0054640252312780444,  -0.046667988130649173,   0.0047346914623733714,
        -0.17712686172280406,   0.0016205080004247200,   -0.22703114241338604,
        -0.0028145845916205865, -0.13123089730288540,    -0.0056345646688793190,
        0.078818395388692147,   -0.0037868875028868542,  0.23226434602675575,
        0.0021116338645426574,  0.13985510526186795,     0.0059365649669377071,
        -0.13918475289368595,   0.0018580676740836374,   -0.23254376365752788,
        -0.0054103844866927788, 0.056840618403875359,    -0.0035227192748552472,
        0.25667909012207590,    0.0055029673963982112,   -0.073334392714092062,
        0.0027810273357488265,  -0.24912792167850559,    -0.0069524866497120566,
        0.21288810409948347,    0.0017810057298691725,   0.088760951809475269,
        0.0055957188940032095,  -0.31937177676259115,    -0.0087539416335935556,
        0.34748800245527145,    0.0075378299617709235,   -0.25116537379803394,
        -0.0047285674679876204, 0.13970502851683486,     0.0023672533925155220,
        -0.063649401080083698,  -0.00098993213448982727, 0.024597838934816905,
        0.00035553235917057483, -0.0082485030307578155,  -0.00011176184313622549,
        0.0024390574829093264,  0.000031180384864488629, -0.00064373524734389861,
        -7.8036008952377965e-6, 0.00015310015145922058,  1.7670804180220134e-6,
        -0.000033066844379476900, -3.6460909134279425e-7, 6.5276969021754105e-6,
        6.8957843666189918e-8,  -1.1842811187642386e-6,  -1.2015133285307312e-8,
        1.9839339947648331e-7,  1.9372045971100854e-9,   -3.0815418032523593e-8,
        -2.9013806338735810e-10, 4.4540904298173700e-9,  4.0505136697916078e-11,
        -6.0104912807134771e-10, -5.2873323696828491e-12, 7.5943206779351725e-11,
        6.4679566322060472e-13, -9.0081200925539902e-12, -7.4396949275292252e-14,
        1.0057423059167244e-12, 8.1701187638005194e-15,  -1.0611736208855373e-13,
        -8.9597492970451533e-16, 1.1421575296031385e-14};

    // Force flush — see bootstrap.cpp flush_haze_trace for rationale.
    // Only safe at boundaries where each Ct on the live set is read exactly
    // once next — otherwise the second read sees an evicted shadow.
    auto flush_pair = [](const Ct &a, const Ct &b) {
        std::uint64_t scratch = 0;
        REQUIRE(hazeMemcpy(&scratch, a.c0().as_const().data()[0],
                           sizeof(scratch), HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(&scratch, b.c0().as_const().data()[0],
                           sizeof(scratch), HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    };

    if (fully_packed) {
        // Fully-packed path (slots == N/2): conjugate-split into real+imag,
        // two Chebyshevs, recombine. Mirror OpenFHE's SUB-then-ADD order
        // (ckksrns-fhe.cpp:701-702).
        Ct conj = conjugate(ctx, ct, bk.conjugation_key);
        Ct ctEncI = sub(ctx, ct, conj);
        Ct ctEnc = add(ctx, ct, conj);
        ctEncI = mult_monomial(ctx, ctEncI, 3 * bk.params.slots);
        if (ctEnc.noise_scale_deg() == 2) {
            ctEnc = rescale(ctx, ctEnc);
            ctEncI = rescale(ctx, ctEncI);
        }
        // Safe boundary: ct and conj are no longer needed; ctEnc and
        // ctEncI are each read exactly once next (by Cheby).
        flush_pair(ctEnc, ctEncI);

        ctEnc = eval_chebyshev_series(ctx, ctEnc, coefficients);
        ctEncI = eval_chebyshev_series(ctx, ctEncI, coefficients);
        // Safe boundary: ctEnc and ctEncI are next read exactly once by
        // rescale, then DA, then mono/add. Each read consumes one shadow.
        flush_pair(ctEnc, ctEncI);

    // OpenFHE FIXEDAUTO: unconditional ModReduce after Chebyshev
    // (ckksrns-fhe.cpp:724-725) regardless of NSD. Drops a level on each
    // half before double-angle picks up.
    if (ctx.mode != lbcrypto::FIXEDMANUAL) {
        ctEnc = rescale(ctx, ctEnc);
        ctEncI = rescale(ctx, ctEncI);
    }
    flush_pair(ctEnc, ctEncI);

    // Double-angle iterations: R_UNIFORM=6 for UNIFORM_TERNARY, R_SPARSE=3
    // otherwise — same source as the sparse path (ckksrns-fhe.cpp:727-731).
    // The previous hardcoded 3 silently diverged for UNIFORM_TERNARY (the
    // niobium full-slot config).
    const std::uint32_t numIter = bk.params.double_angle_iterations > 0
                                      ? bk.params.double_angle_iterations
                                      : 3;
    apply_double_angle_iterations(ctx, ctEnc, numIter);
    apply_double_angle_iterations(ctx, ctEncI, numIter);
    flush_pair(ctEnc, ctEncI);

    // Multiply ctEncI by i via MultByMonomial(slots).
    ctEncI = mult_monomial(ctx, ctEncI, bk.params.slots);
    Ct combined = add(ctx, ctEnc, ctEncI);

    // Match OpenFHE: scalar = round(2^deg) where
    //   deg = round(log2(qDouble / 2^plaintextModulus))
    // Computed dynamically from crypto params.
    auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::uint64_t scalar =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    if (scalar != 1)
        combined = mult_int_scalar(ctx, combined, scalar);

    // OpenFHE FIXEDAUTO does one more ModReduce before StC
    // (ckksrns-fhe.cpp:757) so the StC linear-transform input lines up
    // with the precomputed StC plaintext level.
    if (ctx.mode != lbcrypto::FIXEDMANUAL)
        combined = rescale(ctx, combined);
    return combined;
    }

    // Sparsely-packed path (slots < N/2): conjugate-add, one Chebyshev,
    // one double-angle, integer scaling, then a final ModReduce so the
    // StC input lines up with the precomputed StC plaintext level.
    // Mirrors ckksrns-fhe.cpp:786-842.
    Ct ctEnc = add(ctx, ct, conjugate(ctx, ct, bk.conjugation_key));
    // OpenFHE ckksrns-fhe.cpp:791-799:
    //   FIXEDMANUAL: while (NSD > 1) cc->ModReduceInPlace(ctxtEnc);
    //   else:        if (NSD == 2) ModReduceInternalInPlace(ctxtEnc, cd);
    if (ctx.mode == lbcrypto::FIXEDMANUAL) {
        while (ctEnc.noise_scale_deg() > 1)
            ctEnc = rescale(ctx, ctEnc);
    } else if (ctEnc.noise_scale_deg() == 2) {
        ctEnc = rescale(ctx, ctEnc);
    }

    ctEnc = eval_chebyshev_series(ctx, ctEnc, coefficients);

    if (ctx.mode != lbcrypto::FIXEDMANUAL)
        ctEnc = rescale(ctx, ctEnc);
    // R_UNIFORM=6 for UNIFORM_TERNARY, R_SPARSE=3 for SPARSE_*. Set in
    // make_bootstrap_keys via bk.params.double_angle_iterations.
    const std::uint32_t num_iter = bk.params.double_angle_iterations > 0
                                       ? bk.params.double_angle_iterations
                                       : 3;
    apply_double_angle_iterations(ctx, ctEnc, num_iter);

    auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(cp);
    const double qDouble =
        cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
    const double powP = std::pow(2.0, cp->GetPlaintextModulus());
    const std::int32_t deg =
        static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
    const std::uint64_t scalar =
        static_cast<std::uint64_t>(std::llround(std::pow(2.0, deg)));
    if (scalar != 1)
        ctEnc = mult_int_scalar(ctx, ctEnc, scalar);

    if (ctx.mode != lbcrypto::FIXEDMANUAL)
        ctEnc = rescale(ctx, ctEnc);
    return ctEnc;
}

} // namespace haze::test::ops
