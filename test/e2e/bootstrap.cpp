// Copyright (C) 2026, All rights reserved by Niobium Microsystems.

#include "bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <niobium/openfhe/probes.h>
#include <openfhe.h>
#include <scheme/ckksrns/ckksrns-cryptoparameters.h>
#include <stdexcept>

namespace haze::test::ops {

namespace {

// Suppress OpenFHE-side CPROBES (cprobe_copy / cprobe_move / random / etc.)
// for the duration of ops::bootstrap. The haze helpers here legitimately
// call into OpenFHE for plaintext encoding (encode_const_pt, mult_monomial,
// mult_scalar) and metadata lookups, and those calls construct / SetFormat
// DCRTPoly objects that fire copy/move probes. Pre-fix we wrapped each such
// call site in a PausedRecording; that gates trace_writer emission but also
// blocks the haze sr_* emissions inside the same scope, so it has to be
// minimally scoped. Suppressing only the OpenFHE probes (g_suppressed in
// probes.cpp) lets haze's own sr_* land normally throughout the recording
// while keeping the OpenFHE-side compute completely out of the trace —
// exactly what the test harness wants, with one RAII at the entry point.
struct SuppressOpenFheProbes {
    SuppressOpenFheProbes() noexcept { openfhe_suppress_probes(1); }
    ~SuppressOpenFheProbes() noexcept { openfhe_suppress_probes(0); }
    SuppressOpenFheProbes(const SuppressOpenFheProbes &) = delete;
    SuppressOpenFheProbes &operator=(const SuppressOpenFheProbes &) = delete;
    SuppressOpenFheProbes(SuppressOpenFheProbes &&) = delete;
    SuppressOpenFheProbes &operator=(SuppressOpenFheProbes &&) = delete;
};

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
    SuppressOpenFheProbes _suppress;
    switch (variant) {
    case BootstrapVariant::Standard: {
        // Mirror OpenFHE's bootstrap pipeline (ckksrns-fhe.cpp:586-805).
        // AdjustCiphertext (line 590) has modReduce=true as default. Per-mode:
        //  - FIXEDMANUAL / FIXEDAUTO: EvalMult(ct, 2^-correction) + ModReduce.
        //  - FLEXIBLEAUTO / FLEXIBLEAUTOEXT: EvalMult by adjustmentFactor =
        //    (targetSF/sourceSF)*(modToDrop/sourceSF)*2^-correction, then
        //    ModReduce, then SetScalingFactor(targetSF). `lvl` arg is 1 for
        //    FLEXIBLEAUTOEXT, 0 for the others.
        auto cp = std::dynamic_pointer_cast<lbcrypto::CryptoParametersCKKSRNS>(
            ctx.cc->GetCryptoParameters());
        REQUIRE(cp);
        const double qDouble =
            cp->GetElementParams()->GetParams()[0]->GetModulus().ConvertToDouble();
        const double powP = std::pow(2.0, cp->GetPlaintextModulus());
        const std::int32_t deg =
            static_cast<std::int32_t>(std::round(std::log2(qDouble / powP)));
        const std::int32_t correction_factor = 11;
        const std::int32_t correction = correction_factor - deg;
        const double pre = 1.0 / std::pow(2.0, static_cast<double>(deg));
        constexpr std::uint32_t K_UNIFORM = 512;
        const std::uint32_t N = static_cast<std::uint32_t>(ctx.ring_dim);
        const std::uint32_t lvl =
            (ctx.mode == lbcrypto::FLEXIBLEAUTOEXT) ? 1u : 0u;
        const bool flexible = (ctx.mode == lbcrypto::FLEXIBLEAUTO ||
                               ctx.mode == lbcrypto::FLEXIBLEAUTOEXT);

        // Mirror OpenFHE EvalBootstrap (ckksrns-fhe.cpp:586-590):
        // pre-ModReduce to NSD=1, then AdjustCiphertext (with modReduce=true
        // default). AdjustCiphertext reads sourceSF / numTowers AFTER the
        // pre-ModReduce, so the rescale here must happen first.
        Ct adjusted = [&]() {
            Ct r = clone_ct(ctx, ct);
            while (r.noise_scale_deg() > 1)
                r = rescale(ctx, std::move(r));
            if (flexible) {
                const double targetSF = cp->GetScalingFactorReal(lvl);
                const double sourceSF = r.scaling_factor();
                const std::uint32_t numTowers =
                    static_cast<std::uint32_t>(r.towers());
                const double modToDrop = cp->GetElementParams()
                    ->GetParams()[numTowers - 1]
                    ->GetModulus()
                    .ConvertToDouble();
                const double adjustmentFactor = (targetSF / sourceSF) *
                                                (modToDrop / sourceSF) *
                                                std::pow(2.0, -correction);
                r = mult_by_const_for_test(ctx, r, adjustmentFactor);
                r = rescale(ctx, std::move(r));
                r.set_scaling_factor(targetSF);
                return r;
            }
            r = mult_by_const_for_test(ctx, r, std::pow(2.0, -correction));
            r = rescale(ctx, std::move(r));
            return r;
        }();
        Ct depleted = clone_ct(ctx, adjusted);
        if (depleted.towers() > 1)
            depleted = level_reduce(ctx, std::move(depleted),
                                    depleted.towers() - 1);
        Ct raised = mod_raise(ctx, bk, depleted);
        raised = eval_mult_scalar_for_test(
            ctx, raised, pre / (static_cast<double>(K_UNIFORM) * N));

        if (bk.params.slots < N / 2)
            raised = partial_sum(ctx, bk, std::move(raised));

        raised = rescale(ctx, raised);
        const bool is_lt = (bk.params.level_budget.size() == 2 &&
                            bk.params.level_budget[0] == 1 &&
                            bk.params.level_budget[1] == 1);
        Ct in_slots = is_lt
                          ? linear_transform_v2(ctx, bk, bk.cts_matrices, raised)
                          : eval_coeffs_to_slots(ctx, bk, bk.cts_matrices_fft, raised);
        Ct modded = eval_mod(ctx, bk, in_slots);
        // Defensive: with the right double_angle_iterations count for the
        // secret-key dist (set in make_bootstrap_keys), modded.towers should
        // already match stc_q_towers — the level_reduce here is a no-op.
        // Left in as a guard for parameter combinations that consume fewer
        // levels; remove when ops::bootstrap is mode-aware enough that the
        // condition becomes unreachable.
        const std::size_t stc_q_towers = is_lt
            ? (bk.stc_matrices.front().size() - ctx.p_base.size())
            : (bk.stc_matrices_fft.front().front().size() - ctx.p_base.size());
        if (modded.towers() > stc_q_towers)
            modded = level_reduce(ctx, std::move(modded), modded.towers() - stc_q_towers);
        Ct out = is_lt
                     ? linear_transform_v2(ctx, bk, bk.stc_matrices, modded)
                     : eval_slots_to_coeffs(ctx, bk, bk.stc_matrices_fft, modded);

        // SPARSELY PACKED post-StC: ctxtDec += rot(ctxtDec, slots), then
        // multiply by corFactor = 2^11. Mirrors ckksrns-fhe.cpp:846, 852.
        if (bk.params.slots < N / 2) {
            const std::uint32_t auto_index = ctx.cc->FindAutomorphismIndex(bk.params.slots);
            auto it = bk.rotation_keys.find(auto_index);
            REQUIRE(it != bk.rotation_keys.end());
            Ct rotated = rotate_with_key(ctx, out, it->second);
            out = add(ctx, out, rotated);
        }
        // corFactor = 1 << correction (= correction_factor - deg). For the
        // tested setup with deg=10, correction=1, so corFactor=2.
        const std::uint64_t corFactor =
            static_cast<std::uint64_t>(1) << static_cast<std::uint64_t>(correction);
        out = mult_int_scalar_for_test(ctx, out, corFactor);
        return out;
    }
    case BootstrapVariant::StCFirst: {
        Ct in_coeffs = linear_transform_v2(ctx, bk, bk.stc_matrices, ct);
        Ct raised = mod_raise(ctx, bk, in_coeffs);
        Ct in_slots = linear_transform_v2(ctx, bk, bk.cts_matrices, raised);
        return eval_mod(ctx, bk, in_slots);
    }
    }
    throw std::logic_error("haze::test::ops::bootstrap: unhandled BootstrapVariant");
}

} // namespace haze::test::ops
