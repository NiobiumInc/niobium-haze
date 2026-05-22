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
#include <memory>
#include <openfhe.h>
#include <scheme/ckksrns/ckksrns-fhe.h>
#include <scheme/ckksrns/ckksrns-utils.h>
#include <vector>

namespace haze::test::ops {

namespace {

// Encode a constant value at the level matching `ref_ct` and h2d it.
Allocs encode_const_pt(const OpCtx &ctx, const Ct &ref_ct, double scalar,
                       std::uint32_t noise_scale_deg) {
    using namespace lbcrypto;
    const std::uint32_t level = static_cast<std::uint32_t>(ctx.q_base.size() - ref_ct.towers());
    Plaintext pt =
        ctx.cc->MakeCKKSPackedPlaintext(std::vector<std::complex<double>>(
                                            ctx.cc->GetEncodingParams()->GetBatchSize(),
                                            std::complex<double>(scalar, 0)),
                                        noise_scale_deg, level);
    auto pt_elem = pt->GetElement<DCRTPoly>();
    pt_elem.SetFormat(Format::EVALUATION);
    std::vector<std::vector<std::uint64_t>> chain(ref_ct.towers());
    for (std::size_t t = 0; t < ref_ct.towers(); ++t) {
        const auto &np = pt_elem.GetElementAtIndex(static_cast<usint>(t));
        const auto &vals = np.GetValues();
        chain[t].resize(ctx.ring_dim);
        for (std::size_t i = 0; i < ctx.ring_dim; ++i)
            chain[t][i] = vals[i].template ConvertToInt<std::uint64_t>();
    }
    return Allocs(chain);
}

// EvalMult by a double constant: encode and mult_pt, returns NSD bumped by 1.
Ct mult_by_const(const OpCtx &ctx, const Ct &ct, double scalar) {
    Allocs pt = encode_const_pt(ctx, ct, scalar, 1);
    return mult_pt(ctx, ct, pt);
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
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg()};
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
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg()};
}

// OpenFHE's MultByMonomialInPlace.
Ct mult_monomial(const OpCtx &ctx, const Ct &ct, std::uint32_t power) {
    using namespace lbcrypto;
    const std::uint32_t M = static_cast<std::uint32_t>(2 * ctx.ring_dim);
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

    std::vector<std::vector<std::uint64_t>> chain(ct.towers());
    for (std::size_t t = 0; t < ct.towers(); ++t) {
        const auto &np = monomialDCRT.GetElementAtIndex(static_cast<usint>(t));
        const auto &vals = np.GetValues();
        chain[t].resize(ctx.ring_dim);
        for (std::size_t i = 0; i < ctx.ring_dim; ++i)
            chain[t][i] = vals[i].template ConvertToInt<std::uint64_t>();
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
    // NOT a CKKS-encoded plaintext, so NSD is preserved.
    return Ct{std::move(out_c0), std::move(out_c1), ct.towers(), ct.noise_scale_deg()};
}

Ct square_ct(const OpCtx &ctx, const Ct &ct) { return mult(ctx, ct, ct); }

// Bring `high` (more towers, lower level) down to `low`'s level shape.
// Mirrors the c1lvl < c2lvl branch of LeveledSHECKKSRNS::
// AdjustLevelsAndDepthInPlace (ckksrns-leveledshe.cpp:603-733).
//
// MODE COVERAGE: currently FIXEDAUTO only. compositeDegree=1.
// FLEXIBLEAUTO/EXT and COMPOSITESCALING* need per-level scaling-factor
// tracking on Ct + the full formula; out of scope.
//
// Scaling-factor adjustment factor by case (FIXEDAUTO, scf = m_approxSF):
//   A: h_depth=2,l_depth=2  →  scf2/scf1 * q1/scf = (sf²/sf²)*(sf/sf) = 1
//   B: h_depth=2,l_depth=1  →  (sf/sf²)*(sf/sf)   = 1/sf
//   C: h_depth=1,l_depth=2  →  (sf²/sf)/sf        = 1
//   D: h_depth=1,l_depth=1  →  (sf/sf)/sf         = 1/sf
// Earlier I lazily assumed all four collapse to 1.0; that's only true
// for A and C. Phase 11 caught it on case D.
Ct adjust_one_to_other(const OpCtx &ctx, Ct high, const Ct &low) {
    REQUIRE(ctx.mode == lbcrypto::FIXEDAUTO);
    REQUIRE(high.towers() > low.towers());
    const std::size_t towers_diff = high.towers() - low.towers();
    const std::uint32_t h_depth = high.noise_scale_deg();
    const std::uint32_t l_depth = low.noise_scale_deg();
    constexpr std::size_t cd = 1;

    auto crypto_params = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    REQUIRE(crypto_params);
    const double scf = crypto_params->GetScalingFactorReal(0);
    const double inv_scf = 1.0 / scf;

    if (h_depth == 2 && l_depth == 2) {
        high = mult_by_const(ctx, high, 1.0);
        high = rescale(ctx, std::move(high));
        if (towers_diff > cd)
            high = level_reduce(ctx, std::move(high), towers_diff - cd);
    } else if (h_depth == 2 && l_depth != 2) {
        if (towers_diff == cd) {
            high = rescale(ctx, std::move(high));
        } else {
            high = mult_by_const(ctx, high, inv_scf);
            high = rescale(ctx, std::move(high));
            if (towers_diff > 2 * cd)
                high = level_reduce(ctx, std::move(high), towers_diff - 2 * cd);
            high = rescale(ctx, std::move(high));
        }
    } else if (h_depth != 2 && l_depth == 2) {
        high = mult_by_const(ctx, high, 1.0);
        if (towers_diff > 0)
            high = level_reduce(ctx, std::move(high), towers_diff);
    } else {
        // both depth 1
        high = mult_by_const(ctx, high, inv_scf);
        if (towers_diff > cd)
            high = level_reduce(ctx, std::move(high), towers_diff - cd);
        high = rescale(ctx, std::move(high));
    }
    return high;
}

// Mirror OpenFHE's LeveledSHECKKSRNS::AdjustLevelsAndDepthToOneInPlace
// (ckksrns-leveledshe.cpp:736). Equalize levels via adjust_one_to_other,
// equalize depths via mult_by_const(_, 1.0), then if both NSD=2 ModReduce
// both. Output: a and b at the same level, both at NSD=1.
AdjustedPair adjust_for_mult(const OpCtx &ctx, Ct a, Ct b) {
    if (ctx.mode == lbcrypto::FIXEDMANUAL)
        return {std::move(a), std::move(b)};
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

// Mirror OpenFHE's LeveledSHECKKSRNS::AdjustLevelsAndDepthInPlace
// (ckksrns-leveledshe.cpp:603) — same level alignment as the mult
// variant, just without the To-One ModReduce-both wrap. Used by EvalAdd
// and EvalSub.
AdjustedPair adjust_for_add(const OpCtx &ctx, Ct a, Ct b) {
    if (ctx.mode == lbcrypto::FIXEDMANUAL)
        return {std::move(a), std::move(b)};
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

// Pure tower-count reduction (no ModDown arithmetic). Used for
// level-alignment before add/sub/mult, mirroring OpenFHE's
// LevelReduceInPlace. Distinct from ops::rescale which consumes a level.
Ct drop_levels(const OpCtx &ctx, Ct ct, std::size_t levels) {
    return level_reduce(ctx, std::move(ct), levels);
}

uint32_t poly_degree(const std::vector<double> &v) {
    for (std::size_t i = v.size(); i-- > 0;)
        if (std::abs(v[i]) > 1e-14)
            return static_cast<std::uint32_t>(i);
    return 0;
}

// EvalPartialLinearWSum: Σ T[i] * coeffs[i+1] for i in 0..n-1, only
// non-zero coefficients. Returns a single combined ciphertext.
Ct eval_partial_linear_wsum(const OpCtx &ctx, const std::vector<Ct> &T,
                            const std::vector<double> &coeffs, std::uint32_t n) {
    std::vector<std::size_t> used;
    for (std::uint32_t i = 0; i < n; ++i)
        if (std::abs(coeffs[i + 1]) > 1e-14)
            used.push_back(i);
    REQUIRE(!used.empty());
    Ct acc = mult_by_const(ctx, T[used[0]], coeffs[used[0] + 1]);
    for (std::size_t i = 1; i < used.size(); ++i) {
        Ct term = mult_by_const(ctx, T[used[i]], coeffs[used[i] + 1]);
        acc = add(ctx, acc, term);
    }
    return acc;
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
            // T[2j] = 2*T[j]² - 1. adjustForMult(T[j], T[j]) — both args same.
            auto mp = adjust_for_mult(ctx, clone_ct(ctx, T[i / 2 - 1]),
                                     clone_ct(ctx, T[i / 2 - 1]));
            Ct sq = square_ct(ctx, mp.a);
            Ct doubled = mult_int_scalar(ctx, sq, 2);
            Ct rescaled = maybe_rescale(std::move(doubled));
            T.push_back(add_const(ctx, rescaled, -1.0));
        }
    }
    // Mirror OpenFHE: bring each T[i] to T[k-1]'s level/depth via the
    // real AdjustLevelsAndDepthInPlace dance (mult-by-factor + rescale +
    // level_reduce). Doing this with metadata-only level_reduce produces
    // outputs that diverge from OpenFHE byte-for-byte and underconsumes
    // levels relative to what StC expects.
    for (std::uint32_t i = 0; i + 1 < k; ++i) {
        if (T[i].towers() > T.back().towers())
            T[i] = adjust_one_to_other(ctx, std::move(T[i]), T.back());
        if (T[i].noise_scale_deg() < T.back().noise_scale_deg())
            T[i] = mult_by_const(ctx, T[i], 1.0);
    }

    std::vector<Ct> T2;
    T2.reserve(m);
    T2.push_back(clone_ct(ctx, T.back()));
    Ct T2km1 = clone_ct(ctx, T.back());
    for (std::uint32_t i = 1; i < m; ++i) {
        auto sq_in = adjust_for_mult(ctx, clone_ct(ctx, T2[i - 1]),
                                     clone_ct(ctx, T2[i - 1]));
        Ct sq = square_ct(ctx, sq_in.a);
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
// niobium's InnerEvalChebyshevPS_NB (ckksrns-advancedshe.cpp:666).
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
                if (std::abs(divcs->q[1] - 1.0) > 1e-14)
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
    Ct inner = inner_eval_chebyshev_ps(ctx, x, f2, k, m, tree.T, tree.T2);

    // result = inner - T2km1
    Ct t2km1 = clone_ct(ctx, tree.T2km1);
    if (t2km1.towers() > inner.towers())
        t2km1 = drop_levels(ctx, std::move(t2km1), t2km1.towers() - inner.towers());
    else if (inner.towers() > t2km1.towers())
        inner = drop_levels(ctx, std::move(inner), inner.towers() - t2km1.towers());
    return sub(ctx, inner, t2km1);
}

void apply_double_angle_iterations(const OpCtx &ctx, Ct &ct, std::uint32_t num_iter) {
    constexpr double twoPi = 2.0 * M_PI;
    for (std::int32_t i = 1 - static_cast<std::int32_t>(num_iter); i <= 0; ++i) {
        const double scalar = -std::pow(twoPi, -std::pow(2.0, i));
        // OpenFHE's EvalSquareInPlace runs AdjustLevelsAndDepthToOneInPlace
        // on its single input pair first — drops the level when NSD=2 — so
        // we must mirror that here. My ops::mult doesn't auto-adjust, so do
        // it explicitly before squaring.
        auto sq_in = adjust_for_mult(ctx, clone_ct(ctx, ct), clone_ct(ctx, ct));
        ct = mult(ctx, sq_in.a, sq_in.b);
        // 2*ct + scalar — avoid add(ct, ct) self-add (trace-output replay
        // divergence).
        Ct doubled = mult_int_scalar(ctx, ct, 2);
        ct = add_const(ctx, doubled, scalar);
        // OpenFHE's trailing cc->ModReduceInPlace is a no-op in FIXEDAUTO
        // (line 733 of ckksrns-fhe.cpp). Level drops come from the NEXT
        // iter's EvalSquareInPlace via AdjustLevelsAndDepthToOneInPlace.
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
AdjustedPair adjust_for_mult_for_test(const OpCtx &ctx, Ct a, Ct b) {
    return adjust_for_mult(ctx, std::move(a), std::move(b));
}
AdjustedPair adjust_for_add_for_test(const OpCtx &ctx, Ct a, Ct b) {
    return adjust_for_add(ctx, std::move(a), std::move(b));
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

    if (fully_packed) {
        // Fully-packed path (slots == N/2): conjugate-split into real+imag,
        // two Chebyshevs, recombine. Not exercised by phase 5/7 (slots=8,
        // N=2048 falls into sparsely-packed) but kept for completeness.
        Ct conj = conjugate(ctx, ct, bk.conjugation_key);
        Ct ctEnc = add(ctx, ct, conj);
        Ct ctEncI = sub(ctx, ct, conj);
        ctEncI = mult_monomial(ctx, ctEncI, 3 * bk.params.slots);
        if (ctEnc.noise_scale_deg() == 2) {
            ctEnc = rescale(ctx, ctEnc);
            ctEncI = rescale(ctx, ctEncI);
        }

        ctEnc = eval_chebyshev_series(ctx, ctEnc, coefficients);
        ctEncI = eval_chebyshev_series(ctx, ctEncI, coefficients);

    // OpenFHE FIXEDAUTO: unconditional ModReduce after Chebyshev
    // (ckksrns-fhe.cpp:724-725) regardless of NSD. Drops a level on each
    // half before double-angle picks up.
    if (ctx.mode != lbcrypto::FIXEDMANUAL) {
        ctEnc = rescale(ctx, ctEnc);
        ctEncI = rescale(ctx, ctEncI);
    }

    // Double-angle iterations (3 for UNIFORM_TERNARY, K=16).
    constexpr std::uint32_t numIter = 3;
    apply_double_angle_iterations(ctx, ctEnc, numIter);
    apply_double_angle_iterations(ctx, ctEncI, numIter);

    // Multiply ctEncI by i via MultByMonomial(slots).
    ctEncI = mult_monomial(ctx, ctEncI, bk.params.slots);
    Ct combined = add(ctx, ctEnc, ctEncI);

    // Final scalar multiply: K * postscalar. For FIXEDAUTO bootstrap with
    // correctionFactor=11, scalar = K * 2^correction = 16 * 2048 = 32768.
    constexpr std::uint64_t K = 16;
    constexpr std::uint64_t correction = 11;
    const std::uint64_t scalar = K * (1ULL << correction);
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
    // NSD-gated to mirror OpenFHE line 797 — never drop NSD below 1.
    if (ctx.mode != lbcrypto::FIXEDMANUAL && ctEnc.noise_scale_deg() == 2)
        ctEnc = rescale(ctx, ctEnc);

    ctEnc = eval_chebyshev_series(ctx, ctEnc, coefficients);

    if (ctx.mode != lbcrypto::FIXEDMANUAL)
        ctEnc = rescale(ctx, ctEnc);
    apply_double_angle_iterations(ctx, ctEnc, 3);

    constexpr std::uint64_t K = 16;
    ctEnc = mult_int_scalar(ctx, ctEnc, K);

    if (ctx.mode != lbcrypto::FIXEDMANUAL)
        ctEnc = rescale(ctx, ctEnc);
    return ctEnc;
}

} // namespace haze::test::ops
