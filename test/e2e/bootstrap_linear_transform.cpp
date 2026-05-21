// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Hoisted baby-step / giant-step linear transform that mirrors OpenFHE's
// FHECKKSRNS::EvalLinearTransform (ckksrns-fhe.cpp:1860). Built directly
// from haze C-ABI primitives (hazeMulMrp / hazeAddMrp / hazeAutomorphMrp /
// hazeNTTMrp / hazeINTTMrp / hazeModUp / hazeModDown), so the recorded IR
// is the same as any other haze op sequence — no new opcodes.

#include "bootstrap.hpp"
#include "ops.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <optional>
#include <vector>

namespace haze::test::ops {

namespace {

// Extended-basis ciphertext: (b, a) each represented over `towers` Q-rows
// followed by all P-rows, in EVALUATION form.
struct CtExt {
    Allocs b;
    Allocs a;
    std::size_t q_towers; // Q-rows only; total = q_towers + |P|.
};

// Build the |Q| / |P| layouts and digit decomposition needed for the
// hoisted keyswitch math at the given `q_towers` Q-level.
struct ExtLayout {
    std::vector<uint64_t> q_subbase;
    std::vector<uint64_t> qp_base;
    std::vector<uint64_t> digit_bases_flat;
    std::vector<std::size_t> digit_base_lens;
    std::size_t num_digits{};
    std::size_t qp_total{};
};

ExtLayout build_ext_layout(const OpCtx &ctx, std::size_t q_towers,
                           std::size_t num_part_q) {
    ExtLayout L;
    L.q_subbase.assign(ctx.q_base.begin(),
                       ctx.q_base.begin() + static_cast<std::ptrdiff_t>(q_towers));
    L.qp_base = L.q_subbase;
    L.qp_base.insert(L.qp_base.end(), ctx.p_base.begin(), ctx.p_base.end());
    L.qp_total = L.qp_base.size();

    const std::size_t alpha = (ctx.q_base.size() + num_part_q - 1) / num_part_q;
    L.digit_base_lens.reserve(num_part_q);
    for (std::size_t part = 0; part < num_part_q; ++part) {
        const std::size_t start = part * alpha;
        if (start >= q_towers)
            break;
        const std::size_t end = std::min(start + alpha, q_towers);
        L.digit_base_lens.push_back(end - start);
        for (std::size_t i = start; i < end; ++i)
            L.digit_bases_flat.push_back(ctx.q_base[i]);
    }
    L.num_digits = L.digit_base_lens.size();
    return L;
}

// EvalFastRotationPrecompute: ModUp(INTT(c1)) → digits, then NTT each
// digit back to EVALUATION form. Output: flat Allocs of size
// num_digits * qp_total, in EVALUATION form at Q∥P.
Allocs precompute_digits(const OpCtx &ctx, const Allocs &c1, std::size_t q_towers,
                         const ExtLayout &L) {
    Allocs c1_coeff(q_towers, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(c1_coeff.data(), c1.as_const().data(), L.q_subbase.data(),
                        L.q_subbase.size(), nullptr) == HAZE_SUCCESS);

    const hazeModUpParams modup_params = {
        .src_base = L.q_subbase.data(),
        .src_base_len = L.q_subbase.size(),
        .digit_bases = L.digit_bases_flat.data(),
        .digit_bases_total_len = L.digit_bases_flat.size(),
        .digit_base_lens = L.digit_base_lens.data(),
        .digit_count = L.num_digits,
        .p_base = ctx.p_base.data(),
        .p_base_len = ctx.p_base.size(),
    };
    Allocs digits_coeff(L.num_digits * L.qp_total, ctx.poly_bytes);
    REQUIRE(hazeModUp(digits_coeff.data(), c1_coeff.as_const().data(), &modup_params, nullptr) ==
            HAZE_SUCCESS);

    Allocs digits_eval(L.num_digits * L.qp_total, ctx.poly_bytes);
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        std::vector<const void *> in(L.qp_total);
        std::vector<void *> out(L.qp_total);
        for (std::size_t t = 0; t < L.qp_total; ++t) {
            in[t] = digits_coeff.data()[(d * L.qp_total) + t];
            out[t] = digits_eval.data()[(d * L.qp_total) + t];
        }
        REQUIRE(hazeNTTMrp(out.data(), in.data(), L.qp_base.data(), L.qp_base.size(), nullptr) ==
                HAZE_SUCCESS);
    }
    return digits_eval;
}

// Trim a full Q∥P keyswitch key to (first q_towers Q-rows + all P-rows)
// for the current level. Returns per-digit (a-rows, b-rows) Allocs.
struct TrimmedKey {
    std::vector<Allocs> a_per_digit;
    std::vector<Allocs> b_per_digit;
};

TrimmedKey trim_key(const OpCtx &ctx, const haze::HybridKeyswitchLimbs &key,
                    std::size_t q_towers, const ExtLayout &L) {
    TrimmedKey out;
    out.a_per_digit.reserve(L.num_digits);
    out.b_per_digit.reserve(L.num_digits);
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        std::vector<std::vector<uint64_t>> a_rows(L.qp_total);
        std::vector<std::vector<uint64_t>> b_rows(L.qp_total);
        for (std::size_t t = 0; t < q_towers; ++t) {
            a_rows[t] = key.a_limbs[d][t];
            b_rows[t] = key.b_limbs[d][t];
        }
        for (std::size_t t = 0; t < ctx.p_base.size(); ++t) {
            const std::size_t orig = ctx.q_base.size() + t;
            a_rows[q_towers + t] = key.a_limbs[d][orig];
            b_rows[q_towers + t] = key.b_limbs[d][orig];
        }
        out.a_per_digit.emplace_back(a_rows);
        out.b_per_digit.emplace_back(b_rows);
    }
    return out;
}

// Apply rotation/relin key to precomputed digits → (b_ext, a_ext) at Q∥P
// in EVALUATION form. Does NOT ModDown — keeps the result in the
// extended basis so multiple rotations can be summed before a final
// ModDown.
CtExt keyswitch_ext(const OpCtx &ctx, const Allocs &digits_eval, const TrimmedKey &key,
                    std::size_t q_towers, const ExtLayout &L) {
    Allocs accum_a(L.qp_total, ctx.poly_bytes);
    Allocs accum_b(L.qp_total, ctx.poly_bytes);
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        std::vector<const void *> dig(L.qp_total);
        for (std::size_t t = 0; t < L.qp_total; ++t)
            dig[t] = digits_eval.data()[(d * L.qp_total) + t];
        if (d == 0) {
            REQUIRE(hazeMulMrp(accum_b.data(), dig.data(),
                               key.b_per_digit[d].as_const().data(), L.qp_base.data(),
                               L.qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeMulMrp(accum_a.data(), dig.data(),
                               key.a_per_digit[d].as_const().data(), L.qp_base.data(),
                               L.qp_base.size(), nullptr) == HAZE_SUCCESS);
        } else {
            Allocs prod_b(L.qp_total, ctx.poly_bytes);
            Allocs prod_a(L.qp_total, ctx.poly_bytes);
            REQUIRE(hazeMulMrp(prod_b.data(), dig.data(),
                               key.b_per_digit[d].as_const().data(), L.qp_base.data(),
                               L.qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeMulMrp(prod_a.data(), dig.data(),
                               key.a_per_digit[d].as_const().data(), L.qp_base.data(),
                               L.qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeAddMrp(accum_b.data(), accum_b.as_const().data(),
                               prod_b.as_const().data(), L.qp_base.data(), L.qp_base.size(),
                               nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeAddMrp(accum_a.data(), accum_a.as_const().data(),
                               prod_a.as_const().data(), L.qp_base.data(), L.qp_base.size(),
                               nullptr) == HAZE_SUCCESS);
        }
    }
    return CtExt{.b = std::move(accum_b), .a = std::move(accum_a), .q_towers = q_towers};
}

// Apply automorphism to an extended ciphertext in place at Q∥P.
void automorph_ext_inplace(const OpCtx &ctx, CtExt &c, std::uint32_t auto_index,
                           const ExtLayout &L) {
    Allocs out_b(L.qp_total, ctx.poly_bytes);
    Allocs out_a(L.qp_total, ctx.poly_bytes);
    REQUIRE(hazeAutomorphMrp(out_b.data(), c.b.as_const().data(),
                             static_cast<std::uint64_t>(auto_index), L.qp_base.data(),
                             L.qp_base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAutomorphMrp(out_a.data(), c.a.as_const().data(),
                             static_cast<std::uint64_t>(auto_index), L.qp_base.data(),
                             L.qp_base.size(), nullptr) == HAZE_SUCCESS);
    c.b = std::move(out_b);
    c.a = std::move(out_a);
}

// Mirror OpenFHE's KeySwitchExt: multiply Q-portion by PModq[t] (the
// product of P-moduli reduced mod q_t), zero the P-portion.
Allocs extend_to_qp(const OpCtx &ctx, const Allocs &q_only, std::size_t q_towers,
                    const ExtLayout &L, const std::vector<std::uint64_t> &p_mod_q) {
    REQUIRE(p_mod_q.size() >= q_towers);
    Allocs ext(L.qp_total, ctx.poly_bytes);
    // Multiply each Q-tower by its PModq scalar.
    std::vector<const void *> src_q(q_towers);
    std::vector<void *> dst_q(q_towers);
    for (std::size_t t = 0; t < q_towers; ++t) {
        src_q[t] = q_only[t];
        dst_q[t] = ext.data()[t];
    }
    REQUIRE(hazeMulScalarMrp(dst_q.data(), src_q.data(), p_mod_q.data(), L.q_subbase.data(),
                             L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    // Zero the P-portion.
    for (std::size_t t = q_towers; t < L.qp_total; ++t) {
        REQUIRE(hazeMemset(ext.data()[t], 0, ctx.poly_bytes) == HAZE_SUCCESS);
    }
    return ext;
}

// Multiply (b_ext, a_ext) by an extended plaintext (Q∥P, EVALUATION form).
CtExt mult_ext_pt(const OpCtx &ctx, const CtExt &c, const Allocs &pt_ext, const ExtLayout &L) {
    REQUIRE(pt_ext.size() == L.qp_total);
    Allocs out_b(L.qp_total, ctx.poly_bytes);
    Allocs out_a(L.qp_total, ctx.poly_bytes);
    REQUIRE(hazeMulMrp(out_b.data(), c.b.as_const().data(), pt_ext.as_const().data(),
                       L.qp_base.data(), L.qp_base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulMrp(out_a.data(), c.a.as_const().data(), pt_ext.as_const().data(),
                       L.qp_base.data(), L.qp_base.size(), nullptr) == HAZE_SUCCESS);
    return CtExt{.b = std::move(out_b), .a = std::move(out_a), .q_towers = c.q_towers};
}

void add_ext_inplace(const OpCtx & /*ctx*/, CtExt &dst, const CtExt &src, const ExtLayout &L) {
    REQUIRE(hazeAddMrp(dst.b.data(), dst.b.as_const().data(), src.b.as_const().data(),
                       L.qp_base.data(), L.qp_base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAddMrp(dst.a.data(), dst.a.as_const().data(), src.a.as_const().data(),
                       L.qp_base.data(), L.qp_base.size(), nullptr) == HAZE_SUCCESS);
}

// KeySwitchDown: ModDown Q∥P → Q. Drops the P-portion via INTT, ModDown,
// NTT back. Returns (b_q, a_q) at q_towers in EVALUATION form.
// NSD argument is the noise scale degree of the input ext ciphertext.
Ct keyswitch_down(const OpCtx &ctx, const CtExt &c, const ExtLayout &L, std::uint32_t nsd) {
    Allocs b_coeff(L.qp_total, ctx.poly_bytes);
    Allocs a_coeff(L.qp_total, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(b_coeff.data(), c.b.as_const().data(), L.qp_base.data(), L.qp_base.size(),
                        nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTTMrp(a_coeff.data(), c.a.as_const().data(), L.qp_base.data(), L.qp_base.size(),
                        nullptr) == HAZE_SUCCESS);

    const hazeModDownParams md = {
        .src_base = L.qp_base.data(),
        .src_base_len = L.qp_base.size(),
        .rescale_base = ctx.p_base.data(),
        .rescale_base_len = ctx.p_base.size(),
    };
    Allocs b_md(c.q_towers, ctx.poly_bytes);
    Allocs a_md(c.q_towers, ctx.poly_bytes);
    REQUIRE(hazeModDown(b_md.data(), b_coeff.as_const().data(), &md, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeModDown(a_md.data(), a_coeff.as_const().data(), &md, nullptr) == HAZE_SUCCESS);

    Allocs b_out(c.q_towers, ctx.poly_bytes);
    Allocs a_out(c.q_towers, ctx.poly_bytes);
    REQUIRE(hazeNTTMrp(b_out.data(), b_md.as_const().data(), L.q_subbase.data(),
                       L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeNTTMrp(a_out.data(), a_md.as_const().data(), L.q_subbase.data(),
                       L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    return Ct{std::move(b_out), std::move(a_out), c.q_towers, nsd};
}

const RotationKeyEntry &lookup_rotation_key(const OpCtx &ctx, const BootstrapKeys &bk,
                                            std::int32_t slot_idx) {
    const std::uint32_t auto_index =
        ctx.cc->FindAutomorphismIndex(static_cast<std::uint32_t>(slot_idx));
    auto it = bk.rotation_keys.find(auto_index);
    REQUIRE(it != bk.rotation_keys.end());
    return it->second;
}

} // namespace

Ct linear_transform(const OpCtx &ctx, const BootstrapKeys &bk,
                    const std::vector<Allocs> &matrices, const Ct &ct) {
    REQUIRE(!matrices.empty());
    const std::size_t slots = matrices.size();
    const std::uint32_t g = bk.params.chebyshev_degree;
    const std::size_t bStep = (g == 0)
                                  ? static_cast<std::size_t>(std::ceil(std::sqrt(slots)))
                                  : g;
    const std::size_t gStep = (slots + bStep - 1) / bStep;

    const std::size_t q_towers = ct.towers();
    // Pull the partition count from any extracted key (relin or rotation).
    const std::size_t num_part_q = bk.relin_key.a_limbs.size();
    REQUIRE(num_part_q > 0);
    const ExtLayout L = build_ext_layout(ctx, q_towers, num_part_q);

    // Hoist the ModUp on ct.c1 once for the entire baby-step loop.
    Allocs digits = precompute_digits(ctx, ct.c1(), q_towers, L);

    // Precompute the (b_ext, a_ext) for each baby-step rotation. j=0 is
    // identity: (c0_extended, c1_extended) — KeySwitchExt(ct, true).
    std::vector<CtExt> baby_rotations;
    baby_rotations.reserve(bStep);
    {
        Allocs b0 = extend_to_qp(ctx, ct.c0(), q_towers, L, bk.p_mod_q);
        Allocs a0 = extend_to_qp(ctx, ct.c1(), q_towers, L, bk.p_mod_q);
        baby_rotations.push_back(CtExt{.b = std::move(b0), .a = std::move(a0), .q_towers = q_towers});
    }
    for (std::size_t i = 1; i < bStep; ++i) {
        const auto &entry = lookup_rotation_key(ctx, bk, static_cast<std::int32_t>(i));
        TrimmedKey tk = trim_key(ctx, entry.limbs, q_towers, L);
        CtExt rot = keyswitch_ext(ctx, digits, tk, q_towers, L);
        // Add c0 to b (KeySwitchExt's "true" flag).
        Allocs c0_ext = extend_to_qp(ctx, ct.c0(), q_towers, L, bk.p_mod_q);
        REQUIRE(hazeAddMrp(rot.b.data(), rot.b.as_const().data(), c0_ext.as_const().data(),
                           L.qp_base.data(), L.qp_base.size(), nullptr) == HAZE_SUCCESS);
        // Apply automorphism τ_i.
        automorph_ext_inplace(ctx, rot, entry.auto_index, L);
        baby_rotations.push_back(std::move(rot));
    }

    std::optional<Ct> result;
    for (std::size_t j = 0; j < gStep; ++j) {
        CtExt inner = mult_ext_pt(ctx, baby_rotations[0], matrices[j * bStep], L);
        for (std::size_t i = 1; i < bStep; ++i) {
            const std::size_t idx = (j * bStep) + i;
            if (idx >= slots)
                break;
            CtExt prod = mult_ext_pt(ctx, baby_rotations[i], matrices[idx], L);
            add_ext_inplace(ctx, inner, prod, L);
        }
        // After mult_ext_pt the NSD bumps by 1 from the input ct.
        Ct inner_q = keyswitch_down(ctx, inner, L, ct.noise_scale_deg() + 1);
        if (j > 0) {
            inner_q = rotate_with_key(ctx, inner_q,
                                      lookup_rotation_key(ctx, bk,
                                                          static_cast<std::int32_t>(j * bStep)));
        }
        if (!result.has_value())
            result.emplace(std::move(inner_q));
        else
            *result = add(ctx, *result, inner_q);
    }
    return std::move(*result);
}

} // namespace haze::test::ops
