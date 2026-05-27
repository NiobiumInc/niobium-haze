// Copyright (C) 2026, All rights reserved by Niobium Microsystems.

#include "ops.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge_cc.hpp>
#include <memory>
#include <niobium/compiler.h>
#include <openfhe.h>
#include <utility>
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

uint64_t invmod_prime(uint64_t a, uint64_t q) {
    return powmod_u64(a, q - 2, q);
}

// Balanced-mod ([-q_drop/2, q_drop/2)) lift of v_drop into q_target;
// matches OpenFHE SwitchModulus and fhetch center_mod_q_into_p.
uint64_t signed_lift(uint64_t v_drop, uint64_t q_drop, uint64_t q_target) {
    const uint64_t half = q_drop >> 1;
    if (v_drop > half) {
        const uint64_t neg_mag = q_drop - v_drop;
        const uint64_t neg_mod_t = neg_mag % q_target;
        return (neg_mod_t == 0) ? 0 : (q_target - neg_mod_t);
    }
    return v_drop % q_target;
}

// Pause fhetch recording for the lifetime of this object. OpenFHE
// CPROBES emits sr_* IR for plaintext / poly-format conversions; if such
// a conversion runs mid-epoch, those emits would overwrite live haze
// registers. Wrap the OpenFHE-touching block in this guard.
struct PausedRecording {
    PausedRecording() noexcept { niobium::compiler().pause(); }
    ~PausedRecording() noexcept { niobium::compiler().resume(); }
    PausedRecording(const PausedRecording &) = delete;
    PausedRecording &operator=(const PausedRecording &) = delete;
    PausedRecording(PausedRecording &&) = delete;
    PausedRecording &operator=(PausedRecording &&) = delete;
};

// V_rescaled[t] = (V[t] - signed_lift(V_L, q_t)) · (q_L^-1 mod q_t).
std::vector<uint64_t> rescale_scalars(const std::vector<uint64_t> &scalars_full,
                                      const std::vector<uint64_t> &src_base) {
    const std::size_t out_towers = src_base.size() - 1;
    const uint64_t q_L = src_base.back();
    const uint64_t V_L = scalars_full.back();
    std::vector<uint64_t> rescaled(out_towers);
    for (std::size_t t = 0; t < out_towers; ++t) {
        const uint64_t q = src_base[t];
        const uint64_t lifted = signed_lift(V_L, q_L, q);
        const uint64_t diff = (scalars_full[t] + q - lifted) % q;
        const uint64_t inv_qL = invmod_prime(q_L % q, q);
        rescaled[t] = mulmod_u64(diff, inv_qL, q);
    }
    return rescaled;
}

// Rescale dropping the trailing Q-prime; eval-form input at `src_towers`,
// eval-form output at `src_towers - 1`.
//
// OpenFHE's ModReduce stays in evaluation form and only INTTs the single
// dropped tower; the rest of the rescale is linear:
//   z_t = (x_t - convert_{q_L -> q_t}(x_{q_L})) * q_L^{-1}   (mod q_t).
// The subtraction and the per-tower scalar mul commute with the NTT, so doing
// them in eval form is byte-identical to the coefficient-domain rescale_fbc
// (a full INTT(all)->ModDown->NTT round-trip) but transforms only the dropped
// tower instead of every tower. This is the dominant INTT source, and nothing
// in haze pins the kept towers to coefficient form — they stay in eval.
Allocs rescale_chain_one_tower(const OpCtx &ctx, const Allocs &src, std::size_t src_towers) {
    REQUIRE(src_towers >= 2);
    const std::size_t dst_towers = src_towers - 1;
    std::vector<uint64_t> src_base(ctx.q_base.begin(),
                                   ctx.q_base.begin() + static_cast<std::ptrdiff_t>(src_towers));
    std::vector<uint64_t> dst_base(src_base.begin(),
                                   src_base.begin() + static_cast<std::ptrdiff_t>(dst_towers));
    const uint64_t q_L = src_base.back();
    const std::vector<uint64_t> qL_base = {q_L};

    const auto src_ptrs = src.as_const();

    // INTT only the dropped tower (q_L), then fast-base-convert it into the
    // kept towers' coefficient form — exactly rescale_fbc's y term.
    std::vector<const void *> dropped_in = {src_ptrs[src_towers - 1]};
    Allocs dropped_coeff(1, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(dropped_coeff.data(), dropped_in.data(), qL_base.data(), qL_base.size(),
                        nullptr) == HAZE_SUCCESS);

    const hazeBasisConvertParams bc_params = {
        .src_base = qL_base.data(),
        .src_base_len = qL_base.size(),
        .dst_base = dst_base.data(),
        .dst_base_len = dst_base.size(),
    };
    Allocs y_coeff(dst_towers, ctx.poly_bytes);
    REQUIRE(hazeBasisConvert(y_coeff.data(), dropped_coeff.as_const().data(), &bc_params,
                             nullptr) == HAZE_SUCCESS);
    Allocs y_eval(dst_towers, ctx.poly_bytes);
    REQUIRE(hazeNTTMrp(y_eval.data(), y_coeff.as_const().data(), dst_base.data(), dst_base.size(),
                       nullptr) == HAZE_SUCCESS);

    // z_t = (x_t - y_t) * q_L^{-1}, in eval form over the kept towers.
    std::vector<const void *> src_kept(src_ptrs.begin(),
                                       src_ptrs.begin() + static_cast<std::ptrdiff_t>(dst_towers));
    Allocs diff(dst_towers, ctx.poly_bytes);
    REQUIRE(hazeSubMrp(diff.data(), src_kept.data(), y_eval.as_const().data(), dst_base.data(),
                       dst_base.size(), nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> qL_inv(dst_towers);
    for (std::size_t t = 0; t < dst_towers; ++t)
        qL_inv[t] = invmod_prime(q_L % dst_base[t], dst_base[t]);
    Allocs out(dst_towers, ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out.data(), diff.as_const().data(), qL_inv.data(), dst_base.data(),
                             dst_base.size(), nullptr) == HAZE_SUCCESS);
    return out;
}

Allocs mul_chain(const OpCtx &ctx, const Allocs &x, const Allocs &y,
                 const std::vector<uint64_t> &base) {
    Allocs out(base.size(), ctx.poly_bytes);
    REQUIRE(hazeMulMrp(out.data(), x.as_const().data(), y.as_const().data(), base.data(),
                       base.size(), nullptr) == HAZE_SUCCESS);
    return out;
}

Allocs add_chain(const OpCtx &ctx, const Allocs &x, const Allocs &y,
                 const std::vector<uint64_t> &base) {
    Allocs out(base.size(), ctx.poly_bytes);
    REQUIRE(hazeAddMrp(out.data(), x.as_const().data(), y.as_const().data(), base.data(),
                       base.size(), nullptr) == HAZE_SUCCESS);
    return out;
}

struct TensorResult {
    Allocs d0;
    Allocs d1;
    Allocs d2;
};

TensorResult tensor_product(const OpCtx &ctx, const Ct &a, const Ct &b) {
    REQUIRE(a.towers() == b.towers());
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs d0 = mul_chain(ctx, a.c0(), b.c0(), base);
    Allocs t = mul_chain(ctx, a.c0(), b.c1(), base);
    Allocs u = mul_chain(ctx, a.c1(), b.c0(), base);
    Allocs d1 = add_chain(ctx, t, u, base);
    Allocs d2 = mul_chain(ctx, a.c1(), b.c1(), base);
    return TensorResult{.d0 = std::move(d0), .d1 = std::move(d1), .d2 = std::move(d2)};
}

struct KsContribution {
    Allocs b_contrib;
    Allocs a_contrib;
};

// Hybrid keyswitch of `src` (degree-1, EVAL form, first `towers` Q-primes)
// against `key` — same math whether `key` is a relin key (ct×ct) or an
// automorphism key (rotation). Returns the (b, a) contribution in EVAL
// form at `towers` Q-primes.
KsContribution hybrid_keyswitch(const OpCtx &ctx, const Allocs &src, std::size_t towers,
                                const haze::HybridKeyswitchLimbs &key) {
    const std::size_t num_part_q = key.a_limbs.size();
    REQUIRE(num_part_q > 0);
    const std::size_t alpha = (ctx.q_base.size() + num_part_q - 1) / num_part_q;

    std::vector<uint64_t> q_subbase(ctx.q_base.begin(),
                                    ctx.q_base.begin() + static_cast<std::ptrdiff_t>(towers));
    std::vector<uint64_t> qp_base = q_subbase;
    qp_base.insert(qp_base.end(), ctx.p_base.begin(), ctx.p_base.end());

    std::vector<uint64_t> digit_bases_flat;
    std::vector<std::size_t> digit_base_lens;
    digit_base_lens.reserve(num_part_q);
    for (std::size_t part = 0; part < num_part_q; ++part) {
        const std::size_t start = part * alpha;
        if (start >= towers)
            break;
        const std::size_t end = std::min(start + alpha, towers);
        digit_base_lens.push_back(end - start);
        for (std::size_t i = start; i < end; ++i)
            digit_bases_flat.push_back(ctx.q_base[i]);
    }
    const std::size_t num_digits = digit_base_lens.size();
    REQUIRE(num_digits > 0);

    const hazeModUpParams modup_params = {
        .src_base = q_subbase.data(),
        .src_base_len = q_subbase.size(),
        .digit_bases = digit_bases_flat.data(),
        .digit_bases_total_len = digit_bases_flat.size(),
        .digit_base_lens = digit_base_lens.data(),
        .digit_count = num_digits,
        .p_base = ctx.p_base.data(),
        .p_base_len = ctx.p_base.size(),
    };
    const std::size_t qp_towers = qp_base.size();

    Allocs src_coeff(towers, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(src_coeff.data(), src.as_const().data(), q_subbase.data(), q_subbase.size(),
                        nullptr) == HAZE_SUCCESS);

    Allocs digits_flat(num_digits * qp_towers, ctx.poly_bytes);
    REQUIRE(hazeModUp(digits_flat.data(), src_coeff.as_const().data(), &modup_params, nullptr) ==
            HAZE_SUCCESS);

    // ModUp leaves digit d's own-base Q rows as the identity copy of the
    // input's coefficient form, so their eval form is exactly `src` (already
    // available, in eval, as the keyswitch input). Skip the NTT on those rows
    // and feed `src` straight into the MAC below; only the cross-base Q rows
    // and the P rows are genuinely new and need transforming.
    std::vector<std::size_t> own_start(num_digits);
    std::vector<std::size_t> own_end(num_digits);
    for (std::size_t d = 0, acc = 0; d < num_digits; ++d) {
        own_start[d] = acc;
        acc += digit_base_lens[d];
        own_end[d] = acc;
    }
    Allocs digits_eval(num_digits * qp_towers, ctx.poly_bytes);
    auto ntt_digit_range = [&](std::size_t d, std::size_t lo, std::size_t hi) {
        if (lo >= hi)
            return;
        const std::size_t n = hi - lo;
        std::vector<const void *> in(n);
        std::vector<void *> out(n);
        for (std::size_t j = 0; j < n; ++j) {
            in[j] = digits_flat.data()[(d * qp_towers) + lo + j];
            out[j] = digits_eval.data()[(d * qp_towers) + lo + j];
        }
        REQUIRE(hazeNTTMrp(out.data(), in.data(), qp_base.data() + lo, n, nullptr) == HAZE_SUCCESS);
    };
    for (std::size_t d = 0; d < num_digits; ++d) {
        ntt_digit_range(d, 0, own_start[d]);
        ntt_digit_range(d, own_end[d], qp_towers);
    }
    const std::vector<const void *> src_ptrs = src.as_const();

    // Trim the full-Q∥P key to (first `towers` Q-rows, all P-rows) — matters
    // only when towers < |Q|.
    std::vector<Allocs> a_dev_per_digit;
    std::vector<Allocs> b_dev_per_digit;
    a_dev_per_digit.reserve(num_digits);
    b_dev_per_digit.reserve(num_digits);
    for (std::size_t d = 0; d < num_digits; ++d) {
        std::vector<std::vector<uint64_t>> a_trim(qp_towers);
        std::vector<std::vector<uint64_t>> b_trim(qp_towers);
        for (std::size_t t = 0; t < towers; ++t) {
            a_trim[t] = key.a_limbs[d][t];
            b_trim[t] = key.b_limbs[d][t];
        }
        for (std::size_t t = 0; t < ctx.p_base.size(); ++t) {
            const std::size_t orig = ctx.q_base.size() + t;
            a_trim[towers + t] = key.a_limbs[d][orig];
            b_trim[towers + t] = key.b_limbs[d][orig];
        }
        a_dev_per_digit.emplace_back(a_trim);
        b_dev_per_digit.emplace_back(b_trim);
    }

    Allocs accum_a(qp_towers, ctx.poly_bytes);
    Allocs accum_b(qp_towers, ctx.poly_bytes);
    for (std::size_t d = 0; d < num_digits; ++d) {
        std::vector<const void *> dig(qp_towers);
        for (std::size_t t = 0; t < qp_towers; ++t)
            dig[t] = (t >= own_start[d] && t < own_end[d])
                         ? src_ptrs[t]
                         : digits_eval.data()[(d * qp_towers) + t];
        if (d == 0) {
            REQUIRE(hazeMulMrp(accum_b.data(), dig.data(), b_dev_per_digit[d].as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeMulMrp(accum_a.data(), dig.data(), a_dev_per_digit[d].as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
        } else {
            Allocs prod_b(qp_towers, ctx.poly_bytes);
            Allocs prod_a(qp_towers, ctx.poly_bytes);
            REQUIRE(hazeMulMrp(prod_b.data(), dig.data(), b_dev_per_digit[d].as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeMulMrp(prod_a.data(), dig.data(), a_dev_per_digit[d].as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeAddMrp(accum_b.data(), accum_b.as_const().data(), prod_b.as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
            REQUIRE(hazeAddMrp(accum_a.data(), accum_a.as_const().data(), prod_a.as_const().data(),
                               qp_base.data(), qp_base.size(), nullptr) == HAZE_SUCCESS);
        }
    }

    Allocs accum_a_coeff(qp_towers, ctx.poly_bytes);
    Allocs accum_b_coeff(qp_towers, ctx.poly_bytes);
    REQUIRE(hazeINTTMrp(accum_a_coeff.data(), accum_a.as_const().data(), qp_base.data(),
                        qp_base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTTMrp(accum_b_coeff.data(), accum_b.as_const().data(), qp_base.data(),
                        qp_base.size(), nullptr) == HAZE_SUCCESS);

    const hazeModDownParams ks_md_params = {
        .src_base = qp_base.data(),
        .src_base_len = qp_base.size(),
        .rescale_base = ctx.p_base.data(),
        .rescale_base_len = ctx.p_base.size(),
    };
    Allocs md_a(towers, ctx.poly_bytes);
    Allocs md_b(towers, ctx.poly_bytes);
    REQUIRE(hazeModDown(md_a.data(), accum_a_coeff.as_const().data(), &ks_md_params, nullptr) ==
            HAZE_SUCCESS);
    REQUIRE(hazeModDown(md_b.data(), accum_b_coeff.as_const().data(), &ks_md_params, nullptr) ==
            HAZE_SUCCESS);

    Allocs out_a(towers, ctx.poly_bytes);
    Allocs out_b(towers, ctx.poly_bytes);
    REQUIRE(hazeNTTMrp(out_a.data(), md_a.as_const().data(), q_subbase.data(), q_subbase.size(),
                       nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeNTTMrp(out_b.data(), md_b.as_const().data(), q_subbase.data(), q_subbase.size(),
                       nullptr) == HAZE_SUCCESS);

    return KsContribution{.b_contrib = std::move(out_b), .a_contrib = std::move(out_a)};
}

} // namespace

Allocs::Allocs(std::size_t count, std::size_t bytes) {
    ptrs_.assign(count, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(hazeMalloc(&ptrs_[i], bytes) == HAZE_SUCCESS);
    }
}

Allocs::Allocs(const std::vector<std::vector<uint64_t>> &residues) {
    ptrs_.assign(residues.size(), nullptr);
    for (std::size_t i = 0; i < residues.size(); ++i) {
        const std::size_t bytes = residues[i].size() * sizeof(uint64_t);
        REQUIRE(hazeMalloc(&ptrs_[i], bytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(ptrs_[i], residues[i].data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
                HAZE_SUCCESS);
    }
}

Allocs::~Allocs() {
    free_all();
}

Allocs::Allocs(Allocs &&o) noexcept : ptrs_(std::move(o.ptrs_)) {
    o.ptrs_.clear();
}

Allocs &Allocs::operator=(Allocs &&o) noexcept {
    if (this != &o) {
        free_all();
        ptrs_ = std::move(o.ptrs_);
        o.ptrs_.clear();
    }
    return *this;
}

std::vector<const void *> Allocs::as_const() const {
    return {ptrs_.begin(), ptrs_.end()};
}

void Allocs::truncate(std::size_t new_size) noexcept {
    while (ptrs_.size() > new_size) {
        if (ptrs_.back() != nullptr)
            (void)hazeFree(ptrs_.back());
        ptrs_.pop_back();
    }
}

void Allocs::free_all() noexcept {
    // Swallow INVALID_VALUE so a stale post-reset addr doesn't trip the
    // destructor; hazeFree(nullptr) is already a no-op.
    for (void *p : ptrs_) {
        if (p != nullptr)
            (void)hazeFree(p);
    }
    ptrs_.clear();
}

OpCtx make_ctx(const CtxParams &params) {
    using namespace lbcrypto;

    OpCtx ctx;
    ctx.mode = params.mode;
    ctx.with_relin_key = params.with_relin_key;

    CCParams<CryptoContextCKKSRNS> cc_params;
    cc_params.SetMultiplicativeDepth(params.mult_depth);
    cc_params.SetScalingModSize(params.scaling_mod_size);
    cc_params.SetBatchSize(params.batch_size);
    cc_params.SetScalingTechnique(params.mode);
    if (params.ring_dim != 0) {
        cc_params.SetRingDim(params.ring_dim);
        cc_params.SetSecurityLevel(lbcrypto::HEStd_NotSet);
    }
    ctx.cc = GenCryptoContext(cc_params);
    REQUIRE(ctx.cc);
    ctx.cc->Enable(PKE);
    ctx.cc->Enable(KEYSWITCH);
    ctx.cc->Enable(LEVELEDSHE);

    ctx.keys = ctx.cc->KeyGen();
    if (ctx.with_relin_key)
        ctx.cc->EvalMultKeyGen(ctx.keys.secretKey);

    ctx.ring_dim = ctx.cc->GetRingDimension();
    ctx.poly_bytes = static_cast<std::size_t>(ctx.ring_dim) * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(ctx.ring_dim) == HAZE_SUCCESS);
    REQUIRE(haze::hazeReplayBridgeRegisterCryptoContext(ctx.cc) == HAZE_SUCCESS);

    // Any keyswitch key (relin or rotation) needs P-base for Q∥P intermediates.
    const bool needs_p_base = ctx.with_relin_key || !params.rotate_indices.empty();
    const auto &q_eparams = ctx.cc->GetCryptoParameters()->GetElementParams()->GetParams();
    ctx.q_base.reserve(q_eparams.size());
    for (const auto &p : q_eparams)
        ctx.q_base.push_back(p->GetModulus().ConvertToInt());

    if (needs_p_base) {
        const auto rns_params =
            std::dynamic_pointer_cast<CryptoParametersRNS>(ctx.cc->GetCryptoParameters());
        REQUIRE(rns_params);
        const auto &p_eparams = rns_params->GetParamsP()->GetParams();
        ctx.p_base.reserve(p_eparams.size());
        for (const auto &p : p_eparams)
            ctx.p_base.push_back(p->GetModulus().ConvertToInt());
    }

    int mod_idx = 0;
    for (uint64_t q : ctx.q_base) {
        REQUIRE(hazeSetCiphertextModulus(mod_idx++, q) == HAZE_SUCCESS);
    }
    for (uint64_t pmod : ctx.p_base) {
        REQUIRE(hazeSetCiphertextModulus(mod_idx++, pmod) == HAZE_SUCCESS);
    }
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    if (ctx.with_relin_key) {
        REQUIRE(haze::hazeReplayBridgeExtractEvalMultKey(ctx.cc, ctx.keys.secretKey,
                                                         ctx.relin_key) == HAZE_SUCCESS);
        REQUIRE(ctx.relin_key.q_base == ctx.q_base);
        REQUIRE(ctx.relin_key.p_base == ctx.p_base);
    }

    if (!params.rotate_indices.empty()) {
        ctx.cc->EvalAtIndexKeyGen(ctx.keys.secretKey, params.rotate_indices);
        for (std::int32_t slot_idx : params.rotate_indices) {
            RotationKeyEntry entry;
            entry.auto_index = ctx.cc->FindAutomorphismIndex(static_cast<std::uint32_t>(slot_idx));
            REQUIRE(haze::hazeReplayBridgeExtractAutomorphismKey(
                        ctx.cc, ctx.keys.secretKey, entry.auto_index, entry.limbs) == HAZE_SUCCESS);
            REQUIRE(entry.limbs.q_base == ctx.q_base);
            REQUIRE(entry.limbs.p_base == ctx.p_base);
            ctx.rotation_keys.emplace(slot_idx, std::move(entry));
        }
    }

    return ctx;
}

Ct h2d_ct(const OpCtx &ctx, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &src) {
    REQUIRE(src);
    REQUIRE(src->GetElements().size() == 2);

    auto extract_chain = [&](std::size_t elem_idx) {
        const auto &elem = src->GetElements()[elem_idx];
        const std::size_t towers = elem.GetNumOfElements();
        std::vector<std::vector<uint64_t>> chain(towers);
        for (std::size_t t = 0; t < towers; ++t) {
            const auto &np = elem.GetElementAtIndex(static_cast<usint>(t));
            const auto &vals = np.GetValues();
            REQUIRE(vals.GetLength() == ctx.ring_dim);
            chain[t].resize(ctx.ring_dim);
            for (std::size_t i = 0; i < ctx.ring_dim; ++i) {
                chain[t][i] = vals[i].template ConvertToInt<uint64_t>();
            }
        }
        return chain;
    };

    std::vector<std::vector<uint64_t>> c0_data;
    std::vector<std::vector<uint64_t>> c1_data;
    {
        PausedRecording _pause;
        c0_data = extract_chain(0);
        c1_data = extract_chain(1);
    }
    REQUIRE(c0_data.size() == c1_data.size());
    const std::size_t towers = c0_data.size();
    Allocs c0_alloc(c0_data);
    Allocs c1_alloc(c1_data);
    return {std::move(c0_alloc), std::move(c1_alloc), towers,
            static_cast<std::uint32_t>(src->GetNoiseScaleDeg()),
            src->GetScalingFactor(),
            static_cast<std::uint32_t>(src->GetLevel())};
}

CtBytes d2h_ct(const OpCtx &ctx, const Ct &src) {
    REQUIRE(src.c0().size() == src.towers());
    REQUIRE(src.c1().size() == src.towers());
    auto pull_chain = [&](const Allocs &alloc) {
        std::vector<std::vector<uint64_t>> out(src.towers(), std::vector<uint64_t>(ctx.ring_dim));
        for (std::size_t t = 0; t < src.towers(); ++t) {
            REQUIRE(hazeMemcpy(out[t].data(), alloc[t], ctx.poly_bytes,
                               HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        }
        return out;
    };
    CtBytes bytes;
    bytes.c0 = pull_chain(src.c0());
    bytes.c1 = pull_chain(src.c1());
    return bytes;
}

void inject_ct(const OpCtx &ctx, const CtBytes &src,
               lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &shell) {
    REQUIRE(shell);
    REQUIRE(shell->GetElements().size() >= 2);
    REQUIRE(src.c0.size() == src.c1.size());
    const std::size_t towers = src.c0.size();
    auto inject_elem = [&](std::size_t elem_idx, const std::vector<std::vector<uint64_t>> &rows) {
        auto &dcrt = shell->GetElements()[elem_idx];
        auto &towers_vec = dcrt.GetAllElements();
        REQUIRE(towers_vec.size() == towers);
        for (std::size_t t = 0; t < towers; ++t) {
            const auto &limbs = rows[t];
            REQUIRE(limbs.size() == ctx.ring_dim);
            auto &np = towers_vec[t];
            lbcrypto::NativeVector nv(static_cast<usint>(ctx.ring_dim),
                                      lbcrypto::NativeInteger(np.GetModulus()));
            for (std::size_t i = 0; i < ctx.ring_dim; ++i) {
                nv[i] = lbcrypto::NativeInteger(limbs[i]);
            }
            np.SetValues(nv, np.GetFormat());
        }
    };
    PausedRecording _pause;
    inject_elem(0, src.c0);
    inject_elem(1, src.c1);
}

Ct level_reduce(const OpCtx & /*ctx*/, Ct ct, std::size_t levels) {
    REQUIRE(levels <= ct.towers());
    const std::size_t new_towers = ct.towers() - levels;
    Allocs c0 = std::move(ct.c0());
    Allocs c1 = std::move(ct.c1());
    c0.truncate(new_towers);
    c1.truncate(new_towers);
    // Mirrors LeveledSHECKKSRNS::LevelReduceInternalInPlace: SF preserved,
    // level += dropped towers.
    return Ct{std::move(c0), std::move(c1), new_towers, ct.noise_scale_deg(),
              ct.scaling_factor(),
              ct.level() + static_cast<std::uint32_t>(levels)};
}

Ct clone_ct(const OpCtx &ctx, const Ct &src) {
    // hazeMemcpy(D2D) is a silent no-op on trace-output Allocs — the
    // allocator's copy_d2d looks up shadow_data_[src] which doesn't
    // exist for compute-produced polynomials. Use hazeAutomorphMrp with
    // index=1 (identity permutation) — it emits sr_automorph_eval with
    // the COPY_MODULUS sentinel, which the simulator replays as a
    // value-preserving copy. Workaround until haze's D2D path is fixed
    // to emit the proper sr_addps-with-COPY_MODULUS IR.
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(src.towers()));
    Allocs c0(src.towers(), ctx.poly_bytes);
    Allocs c1(src.towers(), ctx.poly_bytes);
    REQUIRE(hazeAutomorphMrp(c0.data(), src.c0().as_const().data(), 1, base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAutomorphMrp(c1.data(), src.c1().as_const().data(), 1, base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    return Ct{std::move(c0), std::move(c1), src.towers(), src.noise_scale_deg(),
              src.scaling_factor(), src.level()};
}

Ct add(const OpCtx &ctx, const Ct &a, const Ct &b) {
    REQUIRE(a.towers() == b.towers());
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs out_c0 = add_chain(ctx, a.c0(), b.c0(), base);
    Allocs out_c1 = add_chain(ctx, a.c1(), b.c1(), base);
    return {std::move(out_c0), std::move(out_c1), a.towers(),
            std::max(a.noise_scale_deg(), b.noise_scale_deg()),
            a.scaling_factor(), a.level()};
}

Ct sub(const OpCtx &ctx, const Ct &a, const Ct &b) {
    REQUIRE(a.towers() == b.towers());
    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs out_c0(base.size(), ctx.poly_bytes);
    Allocs out_c1(base.size(), ctx.poly_bytes);
    REQUIRE(hazeSubMrp(out_c0.data(), a.c0().as_const().data(), b.c0().as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeSubMrp(out_c1.data(), a.c1().as_const().data(), b.c1().as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    return {std::move(out_c0), std::move(out_c1), a.towers(),
            std::max(a.noise_scale_deg(), b.noise_scale_deg()),
            a.scaling_factor(), a.level()};
}

Ct mult_scalar(const OpCtx &ctx, const Ct &a, const lbcrypto::Plaintext &pt) {
    // Mirror OpenFHE's MorphPlaintext (copy + SetFormat(EVALUATION)) to force
    // lazy materialization. SetFormat is CPROBES-instrumented and emits IR,
    // so the extraction runs under PausedRecording.
    std::vector<uint64_t> scalars(a.towers());
    {
        PausedRecording _pause;
        auto pt_elem = pt->GetElement<lbcrypto::DCRTPoly>();
        pt_elem.SetFormat(Format::EVALUATION);
        const std::size_t pt_towers = pt_elem.GetNumOfElements();
        REQUIRE(pt_towers >= a.towers());

        for (std::size_t t = 0; t < a.towers(); ++t) {
            const auto &np = pt_elem.GetElementAtIndex(static_cast<usint>(t));
            const auto &vals = np.GetValues();
            REQUIRE(vals.GetLength() == ctx.ring_dim);
            scalars[t] = vals[0].template ConvertToInt<uint64_t>();
            for (std::size_t i = 1; i < ctx.ring_dim; ++i) {
                REQUIRE(vals[i].template ConvertToInt<uint64_t>() == scalars[t]);
            }
        }
    }

    if (ctx.mode == lbcrypto::FLEXIBLEAUTOEXT) {
        // Mirrors AdjustForMult inside OpenFHE's EvalMult under FLEXIBLEAUTOEXT.
        REQUIRE(a.towers() >= 2);
        std::vector<uint64_t> src_base(
            ctx.q_base.begin(), ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
        const std::size_t out_towers = a.towers() - 1;
        std::vector<uint64_t> dst_base(src_base.begin(),
                                       src_base.begin() + static_cast<std::ptrdiff_t>(out_towers));
        const auto rescaled_scalars = rescale_scalars(scalars, src_base);

        Allocs rescaled_c0 = rescale_chain_one_tower(ctx, a.c0(), a.towers());
        Allocs rescaled_c1 = rescale_chain_one_tower(ctx, a.c1(), a.towers());

        Allocs out_c0(out_towers, ctx.poly_bytes);
        Allocs out_c1(out_towers, ctx.poly_bytes);
        REQUIRE(hazeMulScalarMrp(out_c0.data(), rescaled_c0.as_const().data(),
                                 rescaled_scalars.data(), dst_base.data(), dst_base.size(),
                                 nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMulScalarMrp(out_c1.data(), rescaled_c1.as_const().data(),
                                 rescaled_scalars.data(), dst_base.data(), dst_base.size(),
                                 nullptr) == HAZE_SUCCESS);
        // FLEXIBLEAUTOEXT pre-rescale path: SF /= modReduceFactor, level+=1,
        // then mult bumps SF by pt.SF.
        auto rns_params = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
            ctx.cc->GetCryptoParameters());
        const double modReduceFactor =
            rns_params->GetModReduceFactor(static_cast<std::uint32_t>(a.towers() - 1));
        const double sf_after_rescale = a.scaling_factor() / modReduceFactor;
        const double sf_out = sf_after_rescale * pt->GetScalingFactor();
        return {std::move(out_c0), std::move(out_c1), out_towers, 1,
                sf_out, a.level() + 1};
    }

    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs out_c0(a.towers(), ctx.poly_bytes);
    Allocs out_c1(a.towers(), ctx.poly_bytes);
    REQUIRE(hazeMulScalarMrp(out_c0.data(), a.c0().as_const().data(), scalars.data(), base.data(),
                             base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalarMrp(out_c1.data(), a.c1().as_const().data(), scalars.data(), base.data(),
                             base.size(), nullptr) == HAZE_SUCCESS);
    return {std::move(out_c0), std::move(out_c1), a.towers(),
            static_cast<std::uint32_t>(a.noise_scale_deg() + pt->GetNoiseScaleDeg()),
            a.scaling_factor() * pt->GetScalingFactor(), a.level()};
}

Ct mult(const OpCtx &ctx, const Ct &a, const Ct &b) {
    REQUIRE(ctx.with_relin_key);
    REQUIRE(!ctx.relin_key.a_limbs.empty());

    REQUIRE(a.towers() == b.towers());
    TensorResult tp = tensor_product(ctx, a, b);
    KsContribution ks = hybrid_keyswitch(ctx, tp.d2, a.towers(), ctx.relin_key);

    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs out_c0 = add_chain(ctx, tp.d0, ks.b_contrib, base);
    Allocs out_c1 = add_chain(ctx, tp.d1, ks.a_contrib, base);
    return {std::move(out_c0), std::move(out_c1), a.towers(),
            a.noise_scale_deg() + b.noise_scale_deg(),
            a.scaling_factor() * b.scaling_factor(), a.level()};
}

Ct rescale(const OpCtx &ctx, const Ct &a) {
    // INTT;ModDown;NTT one tower off the end. FIXEDMANUAL callers use this
    // explicitly; FIXEDAUTO callers (bootstrap helpers) use it to align
    // mult_pt-emitted ciphertexts with OpenFHE's post-EvalMult level shape.
    REQUIRE(a.towers() >= 2);
    // noise_scale_deg_ is uint32_t; rescale on NSD==0 underflows to UINT32_MAX
    // and silently corrupts downstream decisions. Fail loudly.
    REQUIRE(a.noise_scale_deg() >= 1);
    Allocs out_c0 = rescale_chain_one_tower(ctx, a.c0(), a.towers());
    Allocs out_c1 = rescale_chain_one_tower(ctx, a.c1(), a.towers());
    // Mirrors LeveledSHECKKSRNS::ModReduceInternalInPlace: SF /= modReduceFactor,
    // level += 1.
    auto rns_params = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
        ctx.cc->GetCryptoParameters());
    const double modReduceFactor = rns_params->GetModReduceFactor(
        static_cast<std::uint32_t>(a.towers() - 1));
    return {std::move(out_c0), std::move(out_c1), a.towers() - 1,
            a.noise_scale_deg() - 1,
            a.scaling_factor() / modReduceFactor, a.level() + 1};
}

Ct rotate_with_key(const OpCtx &ctx, const Ct &a, const RotationKeyEntry &entry) {
    KsContribution ks = hybrid_keyswitch(ctx, a.c1(), a.towers(), entry.limbs);

    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(a.towers()));
    Allocs ks_c0 = add_chain(ctx, a.c0(), ks.b_contrib, base);
    Allocs ks_c1 = std::move(ks.a_contrib);

    Allocs out_c0(a.towers(), ctx.poly_bytes);
    Allocs out_c1(a.towers(), ctx.poly_bytes);
    REQUIRE(hazeAutomorphMrp(out_c0.data(), ks_c0.as_const().data(),
                             static_cast<std::uint64_t>(entry.auto_index), base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAutomorphMrp(out_c1.data(), ks_c1.as_const().data(),
                             static_cast<std::uint64_t>(entry.auto_index), base.data(), base.size(),
                             nullptr) == HAZE_SUCCESS);
    return {std::move(out_c0), std::move(out_c1), a.towers(), a.noise_scale_deg(),
            a.scaling_factor(), a.level()};
}

Ct rotate(const OpCtx &ctx, const Ct &a, std::int32_t slot_index) {
    const auto it = ctx.rotation_keys.find(slot_index);
    REQUIRE(it != ctx.rotation_keys.end());
    return rotate_with_key(ctx, a, it->second);
}

Ct conjugate(const OpCtx &ctx, const Ct &a, const haze::HybridKeyswitchLimbs &conj_key) {
    const std::uint32_t auto_index = static_cast<std::uint32_t>((2 * ctx.ring_dim) - 1);
    RotationKeyEntry entry{.auto_index = auto_index, .limbs = conj_key};
    return rotate_with_key(ctx, a, entry);
}

Ct mult_pt(const OpCtx &ctx, const Ct &a, const Allocs &pt_chain) {
    // Allow pt_chain to be shorter than ct's towers — bootstrap's CtS/StC
    // plaintexts are encoded at the level they're consumed, which is below
    // the ct's level on entry. We mult only the leading towers and emit a
    // ct at that reduced tower count.
    REQUIRE(pt_chain.size() > 0);
    REQUIRE(pt_chain.size() <= a.towers());
    const std::size_t towers = pt_chain.size();

    std::vector<uint64_t> base(ctx.q_base.begin(),
                               ctx.q_base.begin() + static_cast<std::ptrdiff_t>(towers));
    Allocs out_c0(towers, ctx.poly_bytes);
    Allocs out_c1(towers, ctx.poly_bytes);
    REQUIRE(hazeMulMrp(out_c0.data(), a.c0().as_const().data(), pt_chain.as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulMrp(out_c1.data(), a.c1().as_const().data(), pt_chain.as_const().data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    // Caller is responsible for any rescale dictated by ctx.mode.
    return {std::move(out_c0), std::move(out_c1), towers, a.noise_scale_deg() + 1};
}

} // namespace haze::test::ops
