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

uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t q) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % q);
}
uint64_t powmod_u64(uint64_t a, uint64_t e, uint64_t q) {
    uint64_t r = 1 % q;
    a %= q;
    while (e > 0) {
        if (e & 1ULL)
            r = mulmod_u64(r, a, q);
        a = mulmod_u64(a, a, q);
        e >>= 1;
    }
    return r;
}
uint64_t invmod_prime(uint64_t a, uint64_t q) { return powmod_u64(a, q - 2, q); }

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

// Per-digit own-base Q-row span [start, end). ModUp leaves these rows as the
// identity copy of the input coefficients, so their eval form equals the
// keyswitch input c1 — precompute_digits skips NTT-ing them and keyswitch_ext
// reads c1 there instead.
void digit_own_ranges(const ExtLayout &L, std::vector<std::size_t> &start,
                      std::vector<std::size_t> &end) {
    start.assign(L.num_digits, 0);
    end.assign(L.num_digits, 0);
    std::size_t acc = 0;
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        start[d] = acc;
        acc += L.digit_base_lens[d];
        end[d] = acc;
    }
}

// EvalFastRotationPrecompute: ModUp(INTT(c1)) → digits, then NTT each
// digit back to EVALUATION form. Output: flat Allocs of size
// num_digits * qp_total, in EVALUATION form at Q∥P. Each digit's own-base Q
// rows are left unwritten — their eval form is exactly c1, which keyswitch_ext
// substitutes directly, so the NTT on them is elided.
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

    std::vector<std::size_t> own_start;
    std::vector<std::size_t> own_end;
    digit_own_ranges(L, own_start, own_end);

    Allocs digits_eval(L.num_digits * L.qp_total, ctx.poly_bytes);
    auto ntt_range = [&](std::size_t d, std::size_t lo, std::size_t hi) {
        if (lo >= hi)
            return;
        const std::size_t n = hi - lo;
        std::vector<const void *> in(n);
        std::vector<void *> out(n);
        for (std::size_t j = 0; j < n; ++j) {
            in[j] = digits_coeff.data()[(d * L.qp_total) + lo + j];
            out[j] = digits_eval.data()[(d * L.qp_total) + lo + j];
        }
        REQUIRE(hazeNTTMrp(out.data(), in.data(), L.qp_base.data() + lo, n, nullptr) ==
                HAZE_SUCCESS);
    };
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        ntt_range(d, 0, own_start[d]);
        ntt_range(d, own_end[d], L.qp_total);
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
CtExt keyswitch_ext(const OpCtx &ctx, const Allocs &digits_eval, const Allocs &c1,
                    const TrimmedKey &key, std::size_t q_towers, const ExtLayout &L) {
    std::vector<std::size_t> own_start;
    std::vector<std::size_t> own_end;
    digit_own_ranges(L, own_start, own_end);
    const std::vector<const void *> c1_ptrs = c1.as_const();

    Allocs accum_a(L.qp_total, ctx.poly_bytes);
    Allocs accum_b(L.qp_total, ctx.poly_bytes);
    for (std::size_t d = 0; d < L.num_digits; ++d) {
        std::vector<const void *> dig(L.qp_total);
        for (std::size_t t = 0; t < L.qp_total; ++t)
            dig[t] = (t >= own_start[d] && t < own_end[d])
                         ? c1_ptrs[t]
                         : digits_eval.data()[(d * L.qp_total) + t];
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
Ct keyswitch_down(const OpCtx &ctx, const CtExt &c, const ExtLayout &L, std::uint32_t nsd,
                  double sf, std::uint32_t level) {
    // Eval-form ModDown (see hybrid_keyswitch): the (b, a) ext ciphertext is
    // already in eval form, so INTT only the P-part, convert P->Q, NTT that,
    // and do (ext_q - convert) * P^{-1} in eval form on the Q towers. INTTs
    // |P| towers instead of |Q|+|P|. Byte-identical to rescale_fbc(ext, P).
    std::vector<uint64_t> p_inv(c.q_towers);
    for (std::size_t t = 0; t < c.q_towers; ++t) {
        uint64_t prod = 1 % L.q_subbase[t];
        for (uint64_t p : ctx.p_base)
            prod = mulmod_u64(prod, p % L.q_subbase[t], L.q_subbase[t]);
        p_inv[t] = invmod_prime(prod, L.q_subbase[t]);
    }
    const hazeBasisConvertParams bc = {
        .src_base = ctx.p_base.data(),
        .src_base_len = ctx.p_base.size(),
        .dst_base = L.q_subbase.data(),
        .dst_base_len = L.q_subbase.size(),
    };
    auto md_eval = [&](const Allocs &ext) -> Allocs {
        const auto ext_ptrs = ext.as_const();
        std::vector<const void *> p_part(
            ext_ptrs.begin() + static_cast<std::ptrdiff_t>(c.q_towers), ext_ptrs.end());
        Allocs p_coeff(ctx.p_base.size(), ctx.poly_bytes);
        REQUIRE(hazeINTTMrp(p_coeff.data(), p_part.data(), ctx.p_base.data(), ctx.p_base.size(),
                            nullptr) == HAZE_SUCCESS);
        Allocs y_coeff(c.q_towers, ctx.poly_bytes);
        REQUIRE(hazeBasisConvert(y_coeff.data(), p_coeff.as_const().data(), &bc, nullptr) ==
                HAZE_SUCCESS);
        Allocs y_eval(c.q_towers, ctx.poly_bytes);
        REQUIRE(hazeNTTMrp(y_eval.data(), y_coeff.as_const().data(), L.q_subbase.data(),
                           L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
        std::vector<const void *> q_part(
            ext_ptrs.begin(), ext_ptrs.begin() + static_cast<std::ptrdiff_t>(c.q_towers));
        Allocs diff(c.q_towers, ctx.poly_bytes);
        REQUIRE(hazeSubMrp(diff.data(), q_part.data(), y_eval.as_const().data(),
                           L.q_subbase.data(), L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
        Allocs out(c.q_towers, ctx.poly_bytes);
        REQUIRE(hazeMulScalarMrp(out.data(), diff.as_const().data(), p_inv.data(),
                                 L.q_subbase.data(), L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
        return out;
    };
    Allocs b_out = md_eval(c.b);
    Allocs a_out = md_eval(c.a);
    return Ct{std::move(b_out), std::move(a_out), c.q_towers, nsd, sf, level};
}

// Eval-form ModDown applied to a single extended polynomial (b or a). Mirrors
// the per-poly body of keyswitch_down — INTTs only the P-part, basis-converts
// P->Q, NTTs, and does (ext_q - convert)*P^{-1} in eval form on Q towers.
// Used both by keyswitch_down (b and a) and by KeySwitchDownFirstElement
// (just b).
Allocs ksd_single(const OpCtx &ctx, const Allocs &ext, std::size_t q_towers,
                  const ExtLayout &L) {
    std::vector<uint64_t> p_inv(q_towers);
    for (std::size_t t = 0; t < q_towers; ++t) {
        uint64_t prod = 1 % L.q_subbase[t];
        for (uint64_t p : ctx.p_base)
            prod = mulmod_u64(prod, p % L.q_subbase[t], L.q_subbase[t]);
        p_inv[t] = invmod_prime(prod, L.q_subbase[t]);
    }
    const hazeBasisConvertParams bc = {
        .src_base = ctx.p_base.data(),
        .src_base_len = ctx.p_base.size(),
        .dst_base = L.q_subbase.data(),
        .dst_base_len = L.q_subbase.size(),
    };
    const auto ext_ptrs = ext.as_const();
    std::vector<const void *> p_part(
        ext_ptrs.begin() + static_cast<std::ptrdiff_t>(q_towers), ext_ptrs.end());
    Allocs p_coeff(ctx.p_base.size(), ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(p_coeff.data(), p_part.data(), ctx.p_base.data(), ctx.p_base.size(),
                        nullptr) == HAZE_SUCCESS);
    Allocs y_coeff(q_towers, ctx.poly_bytes);
    REQUIRE(hazeBasisConvert(y_coeff.data(), p_coeff.as_const().data(), &bc, nullptr) ==
            HAZE_SUCCESS);
    Allocs y_eval(q_towers, ctx.poly_bytes);
    REQUIRE(hazeNTTMrp(y_eval.data(), y_coeff.as_const().data(), L.q_subbase.data(),
                       L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    std::vector<const void *> q_part(
        ext_ptrs.begin(), ext_ptrs.begin() + static_cast<std::ptrdiff_t>(q_towers));
    Allocs diff(q_towers, ctx.poly_bytes);
    REQUIRE(hazeSubMrp(diff.data(), q_part.data(), y_eval.as_const().data(),
                       L.q_subbase.data(), L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    Allocs out(q_towers, ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out.data(), diff.as_const().data(), p_inv.data(),
                             L.q_subbase.data(), L.q_subbase.size(), nullptr) == HAZE_SUCCESS);
    return out;
}

// Zero an extended (qp_total) Allocs in place.
void zero_ext_inplace(const OpCtx &ctx, Allocs &ext, const ExtLayout &L) {
    for (std::size_t t = 0; t < L.qp_total; ++t)
        REQUIRE(hazeMemset(ext.data()[t], 0, ctx.poly_bytes) == HAZE_SUCCESS);
}

// Pure automorphism on a Q-only chain (no keyswitch).
Allocs automorph_q(const OpCtx &ctx, const Allocs &c, std::uint32_t auto_index,
                   const std::vector<uint64_t> &q_subbase) {
    Allocs out(q_subbase.size(), ctx.poly_bytes);
    REQUIRE(hazeAutomorphMrp(out.data(), c.as_const().data(),
                             static_cast<std::uint64_t>(auto_index), q_subbase.data(),
                             q_subbase.size(), nullptr) == HAZE_SUCCESS);
    return out;
}

void add_q_inplace(const OpCtx & /*ctx*/, Allocs &dst, const Allocs &src,
                   const std::vector<uint64_t> &q_subbase) {
    REQUIRE(hazeAddMrp(dst.data(), dst.as_const().data(), src.as_const().data(),
                       q_subbase.data(), q_subbase.size(), nullptr) == HAZE_SUCCESS);
}

// Mirror cc->KeySwitchExt(ct, addFirst): extends ct to Q∥P via multiplication
// by PModq on the Q rows; P rows are zero. When addFirst=false, the c0 (b)
// component is fully zero (Q rows too).
CtExt key_switch_ext(const OpCtx &ctx, const Ct &ct, const ExtLayout &L,
                     const std::vector<std::uint64_t> &p_mod_q, bool addFirst) {
    const std::size_t q_towers = ct.towers();
    Allocs a_ext = extend_to_qp(ctx, ct.c1(), q_towers, L, p_mod_q);
    Allocs b_ext;
    if (addFirst) {
        b_ext = extend_to_qp(ctx, ct.c0(), q_towers, L, p_mod_q);
    } else {
        b_ext = Allocs(L.qp_total, ctx.poly_bytes);
        zero_ext_inplace(ctx, b_ext, L);
    }
    return CtExt{.b = std::move(b_ext), .a = std::move(a_ext), .q_towers = q_towers};
}

// Mirror cc->EvalFastRotationExt(ct, k, digits, addFirst). k != 0 applies the
// rotation key at slot index k to the precomputed digits, optionally adds the
// extended c0, then automorphs. k == 0 reduces to KeySwitchExt.
CtExt fast_rotation_ext(const OpCtx &ctx, const Ct &ct, const Allocs &digits,
                        const BootstrapKeys &bk, std::int32_t k, bool addFirst,
                        const ExtLayout &L) {
    const std::size_t q_towers = ct.towers();
    if (k == 0)
        return key_switch_ext(ctx, ct, L, bk.p_mod_q, addFirst);
    const std::uint32_t auto_index =
        ctx.cc->FindAutomorphismIndex(static_cast<std::uint32_t>(k));
    auto it = bk.rotation_keys.find(auto_index);
    REQUIRE(it != bk.rotation_keys.end());
    TrimmedKey tk = trim_key(ctx, it->second.limbs, q_towers, L);
    CtExt rot = keyswitch_ext(ctx, digits, ct.c1(), tk, q_towers, L);
    if (addFirst) {
        Allocs c0_ext = extend_to_qp(ctx, ct.c0(), q_towers, L, bk.p_mod_q);
        REQUIRE(hazeAddMrp(rot.b.data(), rot.b.as_const().data(),
                           c0_ext.as_const().data(), L.qp_base.data(), L.qp_base.size(),
                           nullptr) == HAZE_SUCCESS);
    }
    automorph_ext_inplace(ctx, rot, auto_index, L);
    return rot;
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
        CtExt rot = keyswitch_ext(ctx, digits, ct.c1(), tk, q_towers, L);
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
        // SF *= matrix.SF; level preserved (EvalLinearTransform's internal
        // KeySwitchDown doesn't ModReduce). Picking the matrix SF from bk —
        // CtS uses bk.cts_pt_sf, StC uses bk.stc_pt_sf. Both have the same SF
        // value for the test setup; differentiating callers via the matrices
        // pointer is the cleanest disambiguation.
        const bool is_stc = (&matrices == &bk.stc_matrices);
        const double matrix_sf = is_stc ? bk.stc_pt_sf : bk.cts_pt_sf;
        Ct inner_q = keyswitch_down(ctx, inner, L, ct.noise_scale_deg() + 1,
                                    ct.scaling_factor() * matrix_sf, ct.level());
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

// ReduceRotation: collapse a (possibly negative) rotation index modulo slots.
// Mirrors OpenFHE's ckksrns-utils ReduceRotation.
std::int32_t reduce_rotation(std::int32_t index, std::uint32_t slots) {
    const std::int32_t isize = static_cast<std::int32_t>(slots);
    if (index == 0 || index == isize || index == -isize)
        return 0;
    return ((index % isize) + isize) % isize;
}

// One stage of the multi-stage CtS/StC BSGS transform. Mirrors the per-stage
// body of EvalCoeffsToSlots (ckksrns-fhe.cpp:1963-2013) and EvalSlotsToCoeffs
// (2117-2169): hoisted baby rotations via rot_in, giant-step accumulation in
// extended basis via rot_out, c0 tracked in `first`, one final KSD per stage.
Ct eval_linear_stage(const OpCtx &ctx, const BootstrapKeys &bk,
                     const std::vector<Allocs> &A_stage, const Ct &ct,
                     std::size_t num_part_q,
                     const std::vector<std::int32_t> &rot_in,
                     const std::vector<std::int32_t> &rot_out,
                     std::uint32_t g, std::uint32_t b,
                     std::uint32_t numRotations,
                     double matrix_sf) {
    const std::size_t q_towers = ct.towers();
    const ExtLayout L = build_ext_layout(ctx, q_towers, num_part_q);

    Allocs digits = precompute_digits(ctx, ct.c1(), q_towers, L);

    // Hoisted baby rotations: fastRotation[j] = FastRotExt(ct, rot_in[j], digits, true).
    std::vector<CtExt> fastRotation;
    fastRotation.reserve(g);
    for (std::uint32_t j = 0; j < g; ++j)
        fastRotation.push_back(fast_rotation_ext(ctx, ct, digits, bk, rot_in[j], true, L));

    const std::uint32_t out_nsd = ct.noise_scale_deg() + 1;
    const double out_sf = ct.scaling_factor() * matrix_sf;
    const std::uint32_t out_level = ct.level();

    CtExt outer;
    Allocs first;
    bool initialized = false;
    for (std::uint32_t i = 0; i < b; ++i) {
        const std::uint32_t G = g * i;
        CtExt inner = mult_ext_pt(ctx, fastRotation[0], A_stage[G], L);
        for (std::uint32_t j = 1; j < g; ++j) {
            if ((G + j) != numRotations) {
                CtExt prod = mult_ext_pt(ctx, fastRotation[j], A_stage[G + j], L);
                add_ext_inplace(ctx, inner, prod, L);
            }
        }
        if (i == 0) {
            first = ksd_single(ctx, inner.b, q_towers, L);
            zero_ext_inplace(ctx, inner.b, L);
            outer = std::move(inner);
            initialized = true;
        } else {
            if (rot_out[i] != 0) {
                Ct inner_q = keyswitch_down(ctx, inner, L, out_nsd, out_sf, out_level);
                const std::uint32_t auto_idx =
                    ctx.cc->FindAutomorphismIndex(static_cast<std::uint32_t>(rot_out[i]));
                Allocs c0_rot = automorph_q(ctx, inner_q.c0(), auto_idx, L.q_subbase);
                add_q_inplace(ctx, first, c0_rot, L.q_subbase);
                Allocs inner_digits = precompute_digits(ctx, inner_q.c1(), q_towers, L);
                CtExt rot_ext = fast_rotation_ext(ctx, inner_q, inner_digits, bk,
                                                   rot_out[i], false, L);
                add_ext_inplace(ctx, outer, rot_ext, L);
            } else {
                // rot_out[i] == 0: no rotation, add inner directly to outer
                // (with c0 tracked separately so we don't double-count it).
                Allocs first_inc = ksd_single(ctx, inner.b, q_towers, L);
                add_q_inplace(ctx, first, first_inc, L.q_subbase);
                zero_ext_inplace(ctx, inner.b, L);
                add_ext_inplace(ctx, outer, inner, L);
            }
        }
    }
    REQUIRE(initialized);

    Ct result = keyswitch_down(ctx, outer, L, out_nsd, out_sf, out_level);
    add_q_inplace(ctx, result.c0(), first, L.q_subbase);
    return result;
}

// OpenFHE-mirroring multi-stage CoeffsToSlots (ckksrns-fhe.cpp:1912). Iterates
// stages from smax down to stop; if remCollapse > 0, a remainder stage runs
// last (s = 0 = stop). ModReduce between stages.
Ct eval_coeffs_to_slots(const OpCtx &ctx, const BootstrapKeys &bk,
                        const std::vector<std::vector<Allocs>> &A, const Ct &ct) {
    const auto &p = bk.params.enc;
    const std::uint32_t slots = bk.params.slots;
    const std::uint32_t M4 = static_cast<std::uint32_t>(bk.params.cyclotomic_order / 4);

    const std::int32_t flagRem = (p.remCollapse == 0) ? 0 : 1;
    const std::int32_t stop = flagRem ? 0 : -1;

    std::vector<std::vector<std::int32_t>> rot_out(
        p.lvlb, std::vector<std::int32_t>(p.b + p.bRem));
    std::vector<std::vector<std::int32_t>> rot_in(
        p.lvlb, std::vector<std::int32_t>(p.numRotations + 1));
    if (flagRem == 1) rot_in[0].resize(p.numRotationsRem + 1);

    std::int32_t offset = static_cast<std::int32_t>((p.numRotations + 1) / 2) - 1;
    for (std::int32_t s = static_cast<std::int32_t>(p.lvlb) - 1; s > stop; --s) {
        const std::int32_t scale = 1 << ((s - flagRem) * static_cast<std::int32_t>(p.layersCollapse)
                                          + static_cast<std::int32_t>(p.remCollapse));
        for (std::uint32_t i = 0; i < p.b; ++i)
            rot_out[static_cast<std::size_t>(s)][i] = reduce_rotation(scale * static_cast<std::int32_t>(p.g)
                                             * static_cast<std::int32_t>(i), M4);
        for (std::uint32_t j = 0; j < p.g; ++j)
            rot_in[static_cast<std::size_t>(s)][j] = reduce_rotation(scale * (static_cast<std::int32_t>(j) - offset), slots);
    }
    if (flagRem == 1) {
        offset = static_cast<std::int32_t>((p.numRotationsRem + 1) / 2) - 1;
        for (std::uint32_t i = 0; i < p.bRem; ++i)
            rot_out[static_cast<std::size_t>(stop)][i] = reduce_rotation(static_cast<std::int32_t>(p.gRem * i), M4);
        for (std::uint32_t j = 0; j < p.gRem; ++j)
            rot_in[static_cast<std::size_t>(stop)][j] = reduce_rotation(static_cast<std::int32_t>(j) - offset, slots);
    }

    Ct result = clone_ct(ctx, ct);
    const std::size_t num_part_q = bk.relin_key.a_limbs.size();
    const std::int32_t smax = static_cast<std::int32_t>(p.lvlb) - 1;
    // Per-stage scaling factor: each FFT stage's plaintexts encode at a
    // different level so their SF differs in the last bit(s). Using a
    // single bk.cts_pt_sf across stages produces a last-bit drift in the
    // output SF that cascades into wrong per-tower scalars in downstream
    // mult_by_const. Use bk.cts_pt_sf_per_stage[s] when available.
    auto stage_sf = [&](std::size_t idx) -> double {
        return idx < bk.cts_pt_sf_per_stage.size()
                   ? bk.cts_pt_sf_per_stage[idx]
                   : bk.cts_pt_sf;
    };
    for (std::int32_t s = smax; s > stop; --s) {
        if (s != smax)
            result = rescale(ctx, std::move(result));
        result = eval_linear_stage(ctx, bk, A[static_cast<std::size_t>(s)], result, num_part_q,
                                    rot_in[static_cast<std::size_t>(s)], rot_out[static_cast<std::size_t>(s)], p.g, p.b, p.numRotations,
                                    stage_sf(static_cast<std::size_t>(s)));
    }
    if (flagRem == 1) {
        result = rescale(ctx, std::move(result));
        result = eval_linear_stage(ctx, bk, A[static_cast<std::size_t>(stop)], result, num_part_q,
                                    rot_in[static_cast<std::size_t>(stop)], rot_out[static_cast<std::size_t>(stop)],
                                    p.gRem, p.bRem, p.numRotationsRem,
                                    stage_sf(static_cast<std::size_t>(stop)));
    }
    return result;
}

// OpenFHE-mirroring multi-stage SlotsToCoeffs (ckksrns-fhe.cpp:2069). Stages
// run forward (0 to smax), remainder stage last.
Ct eval_slots_to_coeffs(const OpCtx &ctx, const BootstrapKeys &bk,
                        const std::vector<std::vector<Allocs>> &A, const Ct &ct) {
    const auto &p = bk.params.dec;
    const std::uint32_t M4 = static_cast<std::uint32_t>(bk.params.cyclotomic_order / 4);

    const std::int32_t flagRem = (p.remCollapse == 0) ? 0 : 1;
    const std::int32_t smax = static_cast<std::int32_t>(p.lvlb) - flagRem;

    std::vector<std::vector<std::int32_t>> rot_out(
        p.lvlb, std::vector<std::int32_t>(p.b + p.bRem));
    std::vector<std::vector<std::int32_t>> rot_in(
        p.lvlb, std::vector<std::int32_t>(p.numRotations + 1));
    if (flagRem == 1) rot_in[p.lvlb - 1].resize(p.numRotationsRem + 1);

    std::int32_t offset = static_cast<std::int32_t>((p.numRotations + 1) / 2) - 1;
    for (std::int32_t s = 0; s < smax; ++s) {
        const std::int32_t scale = 1 << (s * static_cast<std::int32_t>(p.layersCollapse));
        for (std::uint32_t j = 0; j < p.g; ++j)
            rot_in[static_cast<std::size_t>(s)][j] = reduce_rotation((static_cast<std::int32_t>(j) - offset) * scale, M4);
        for (std::uint32_t i = 0; i < p.b; ++i)
            rot_out[static_cast<std::size_t>(s)][i] = reduce_rotation((static_cast<std::int32_t>(p.g) *
                                              static_cast<std::int32_t>(i)) * scale, M4);
    }
    if (flagRem == 1) {
        const std::int32_t scaleRem = 1 << (smax * static_cast<std::int32_t>(p.layersCollapse));
        const std::int32_t offsetRem = static_cast<std::int32_t>((p.numRotationsRem + 1) / 2) - 1;
        for (std::uint32_t j = 0; j < p.gRem; ++j)
            rot_in[static_cast<std::size_t>(smax)][j] = reduce_rotation((static_cast<std::int32_t>(j) - offsetRem) * scaleRem, M4);
        for (std::uint32_t i = 0; i < p.bRem; ++i)
            rot_out[static_cast<std::size_t>(smax)][i] = reduce_rotation((static_cast<std::int32_t>(p.gRem) *
                                                  static_cast<std::int32_t>(i)) * scaleRem, M4);
    }

    Ct result = clone_ct(ctx, ct);
    const std::size_t num_part_q = bk.relin_key.a_limbs.size();
    // Per-stage scaling factor; see eval_coeffs_to_slots for rationale.
    auto stage_sf = [&](std::size_t idx) -> double {
        return idx < bk.stc_pt_sf_per_stage.size()
                   ? bk.stc_pt_sf_per_stage[idx]
                   : bk.stc_pt_sf;
    };
    for (std::int32_t s = 0; s < smax; ++s) {
        if (s != 0)
            result = rescale(ctx, std::move(result));
        result = eval_linear_stage(ctx, bk, A[static_cast<std::size_t>(s)], result, num_part_q,
                                    rot_in[static_cast<std::size_t>(s)], rot_out[static_cast<std::size_t>(s)], p.g, p.b, p.numRotations,
                                    stage_sf(static_cast<std::size_t>(s)));
    }
    if (flagRem == 1) {
        result = rescale(ctx, std::move(result));
        result = eval_linear_stage(ctx, bk, A[static_cast<std::size_t>(smax)], result, num_part_q,
                                    rot_in[static_cast<std::size_t>(smax)], rot_out[static_cast<std::size_t>(smax)],
                                    p.gRem, p.bRem, p.numRotationsRem,
                                    stage_sf(static_cast<std::size_t>(smax)));
    }
    return result;
}

// OpenFHE-mirroring single-stage linear transform (ckksrns-fhe.cpp:1860
// EvalLinearTransform). Accumulates `result` in the extended (Q∥P) basis
// across all giant steps, tracks the c0 element in a separate `first` (Q
// form), and does ONE final KeySwitchDown with result.c0 += first. Mirrors
// the canonical algorithm op-for-op; works for any slot count (sparse and
// fully-packed) at levelBudget {1,1}.
Ct linear_transform_v2(const OpCtx &ctx, const BootstrapKeys &bk,
                       const std::vector<Allocs> &matrices, const Ct &ct) {
    REQUIRE(!matrices.empty());
    const std::size_t slots = matrices.size();
    const std::uint32_t g = bk.params.chebyshev_degree;
    const std::size_t bStep = (g == 0)
                                  ? static_cast<std::size_t>(std::ceil(std::sqrt(slots)))
                                  : g;
    const std::size_t gStep = (slots + bStep - 1) / bStep;

    const std::size_t q_towers = ct.towers();
    const std::size_t num_part_q = bk.relin_key.a_limbs.size();
    REQUIRE(num_part_q > 0);
    const ExtLayout L = build_ext_layout(ctx, q_towers, num_part_q);

    // Hoist ModUp on ct.c1 for the baby-step loop.
    Allocs digits = precompute_digits(ctx, ct.c1(), q_towers, L);

    // Precompute baby rotations 1..bStep-1: fastRotation[j-1] = FastRotExt(ct, j, addFirst=true).
    std::vector<CtExt> fastRotation;
    fastRotation.reserve(bStep > 0 ? bStep - 1 : 0);
    for (std::size_t j = 1; j < bStep; ++j) {
        fastRotation.push_back(fast_rotation_ext(ctx, ct, digits, bk,
                                                  static_cast<std::int32_t>(j), true, L));
    }

    // Output metadata for the final ciphertext.
    const bool is_stc = (&matrices == &bk.stc_matrices);
    const double matrix_sf = is_stc ? bk.stc_pt_sf : bk.cts_pt_sf;
    const std::uint32_t out_nsd = ct.noise_scale_deg() + 1;
    const double out_sf = ct.scaling_factor() * matrix_sf;
    const std::uint32_t out_level = ct.level();

    CtExt result_ext;
    Allocs first;
    bool initialized = false;

    for (std::size_t j = 0; j < gStep; ++j) {
        // inner = sum_i mult_ext_pt(<KSExt(ct,true) if i==0 else fastRotation[i-1]>, A[bStep*j+i]).
        CtExt inner;
        bool inner_init = false;
        for (std::size_t i = 0; i < bStep; ++i) {
            const std::size_t idx = (bStep * j) + i;
            if (idx >= slots) break;
            CtExt ks_first;  // storage for the i==0 KSExt; lives across the mult.
            const CtExt *src;
            if (i == 0) {
                ks_first = key_switch_ext(ctx, ct, L, bk.p_mod_q, true);
                src = &ks_first;
            } else {
                src = &fastRotation[i - 1];
            }
            CtExt prod = mult_ext_pt(ctx, *src, matrices[idx], L);
            if (!inner_init) {
                inner = std::move(prod);
                inner_init = true;
            } else {
                add_ext_inplace(ctx, inner, prod, L);
            }
        }
        REQUIRE(inner_init);

        if (j == 0) {
            // first = KSDFirstElement(inner) (just the c0/b component of KSD).
            first = ksd_single(ctx, inner.b, q_towers, L);
            // Zero inner.b → result_ext (the c0 is tracked separately).
            zero_ext_inplace(ctx, inner.b, L);
            result_ext = std::move(inner);
            initialized = true;
        } else {
            // KSD inner → Q, accumulate c0 into first, FastRotExt the result
            // back into the extended basis, accumulate into result_ext.
            Ct inner_q = keyswitch_down(ctx, inner, L, out_nsd, out_sf, out_level);
            const std::uint32_t auto_index =
                ctx.cc->FindAutomorphismIndex(static_cast<std::uint32_t>(bStep * j));
            Allocs c0_rot = automorph_q(ctx, inner_q.c0(), auto_index, L.q_subbase);
            add_q_inplace(ctx, first, c0_rot, L.q_subbase);
            Allocs inner_digits = precompute_digits(ctx, inner_q.c1(), q_towers, L);
            CtExt rotated_ext =
                fast_rotation_ext(ctx, inner_q, inner_digits, bk,
                                  static_cast<std::int32_t>(bStep * j), false, L);
            add_ext_inplace(ctx, result_ext, rotated_ext, L);
        }
    }
    REQUIRE(initialized);

    // Final: KSD result_ext → Q, then result.c0 += first.
    Ct result_q = keyswitch_down(ctx, result_ext, L, out_nsd, out_sf, out_level);
    add_q_inplace(ctx, result_q.c0(), first, L.q_subbase);
    return result_q;
}

} // namespace haze::test::ops
