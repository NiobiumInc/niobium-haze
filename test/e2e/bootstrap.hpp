// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// First-class CKKS bootstrap built atop the haze::test::ops:: ciphertext
// abstraction. Records only haze primitive opcodes, so the eventual
// FIDESlib port inherits bootstrap without a backend-specific path.
// BootstrapKeys is built once and reused across every bootstrap() call;
// the FHETCH trace tags each underlying polynomial as input exactly once.

#pragma once

#include "ops.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <openfhe.h>
#include <vector>

namespace haze::test::ops {

enum class BootstrapVariant : std::uint8_t {
    // ModRaise → CoeffsToSlots → EvalMod → SlotsToCoeffs.
    Standard,
    // SlotsToCoeffs → ModRaise → CoeffsToSlots → EvalMod (thin / StC-first).
    // Requires additional input levels relative to Standard; Phase 2.
    StCFirst,
};

struct BootstrapParams {
    std::uint32_t slots{};
    std::vector<std::uint32_t> level_budget;
    std::vector<std::uint32_t> dim1;
    std::uint32_t correction_factor{};
    std::uint32_t chebyshev_degree{};
    std::uint32_t double_angle_iterations{};
    // Cyclotomic order M = 2 * ring_dim; cached so conjugate() doesn't re-query ctx.
    std::uint64_t cyclotomic_order{};
};

// Owns the haze device allocations for every bootstrap input that is
// invariant across calls. void* addresses are stable for its lifetime.
struct BootstrapKeys {
    haze::HybridKeyswitchLimbs relin_key;
    haze::HybridKeyswitchLimbs conjugation_key;
    // Keyed by automorphism index, matching OpenFHE's FindBootstrapRotationIndices.
    std::map<std::uint32_t, RotationKeyEntry> rotation_keys;
    std::vector<Allocs> cts_matrices; // U0hatT rows per BSGS level
    std::vector<Allocs> stc_matrices; // U0 rows per BSGS level
    // Per-plaintext SF (== precom.m_U0hatTPre[i]->GetScalingFactor()) and
    // encoded level. Required to propagate SF through linear_transform in
    // FLEXIBLEAUTO/FLEXIBLEAUTOEXT, where downstream AdjustLevelsAndDepthInPlace
    // formulas consume ct.SF and the matrix's SF is part of the EvalMultExt
    // product semantics.
    double cts_pt_sf{};
    double stc_pt_sf{};
    std::uint32_t cts_pt_level{};
    std::uint32_t stc_pt_level{};
    std::vector<Allocs> eval_mod_coeffs;
    // PModq[t] = (product of P-moduli) mod q_t. Needed by KeySwitchExt
    // to multiply c0/c1 by P before zero-extending the P-portion.
    std::vector<std::uint64_t> p_mod_q;
    BootstrapParams params;
};

// Drives cc->EvalBootstrapSetup + EvalBootstrapKeyGen, then h2d's each
// artifact into haze memory once. Not yet implemented — see follow-up task.
BootstrapKeys make_bootstrap_keys(const OpCtx &ctx,
                                  const lbcrypto::CryptoContext<lbcrypto::DCRTPoly> &cc,
                                  const lbcrypto::PrivateKey<lbcrypto::DCRTPoly> &sk,
                                  std::uint32_t slots,
                                  const std::vector<std::uint32_t> &level_budget = {1, 1});

// Top-level bootstrap. Body is a switch over `variant` composing the helpers below.
Ct bootstrap(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct,
             BootstrapVariant variant = BootstrapVariant::Standard);

// Extend `ct` from its current Q-basis to the full extended chain.
Ct mod_raise(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct);

// Baby-step / giant-step linear transform; caller passes bk.cts_matrices or bk.stc_matrices.
Ct linear_transform(const OpCtx &ctx, const BootstrapKeys &bk,
                    const std::vector<Allocs> &matrices, const Ct &ct);

// OpenFHE-mirroring single-stage linear transform (ckksrns-fhe.cpp:1860
// EvalLinearTransform). Same interface as linear_transform; canonical
// algorithm with extended-basis accumulation + first c0 tracking + single
// final KSD. Replaces linear_transform once byte-validated for all slot
// counts.
Ct linear_transform_v2(const OpCtx &ctx, const BootstrapKeys &bk,
                       const std::vector<Allocs> &matrices, const Ct &ct);

// Chebyshev approximation of sin(2πKx)/(2πK) followed by `r` double-angle iterations.
Ct eval_mod(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct);

// ---------------------------------------------------------------------------
// Phase helpers exposed for unit-tests. Each mirrors a specific OpenFHE op
// and is verified bit-exact independently. Internal to eval_mod otherwise.
// ---------------------------------------------------------------------------

Ct add_const_for_test(const OpCtx &ctx, const Ct &ct, double scalar);
Ct mult_by_const_for_test(const OpCtx &ctx, const Ct &ct, double scalar);
Ct mult_int_scalar_for_test(const OpCtx &ctx, const Ct &ct, std::uint64_t scalar);
Ct mult_monomial_for_test(const OpCtx &ctx, const Ct &ct, std::uint32_t power);
Ct square_ct_for_test(const OpCtx &ctx, const Ct &ct);
void apply_double_angle_for_test(const OpCtx &ctx, Ct &ct, std::uint32_t num_iter);
Ct eval_chebyshev_series_for_test(const OpCtx &ctx, const Ct &x,
                                  const std::vector<double> &coefficients);

// Expose eval_mod for byte-parity testing.
Ct eval_mod_for_test(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct);

// Adjust-pair helpers. adjust_for_mult mirrors OpenFHE's
// AdjustLevelsAndDepthToOneInPlace; adjust_for_add mirrors
// AdjustLevelsAndDepthInPlace. The test variants expose them so the
// e2e suite can byte-compare against OpenFHE's reference behaviour.
struct AdjustedPair {
    Ct a;
    Ct b;
};
AdjustedPair adjust_for_mult_for_test(const OpCtx &ctx, Ct a, Ct b);
AdjustedPair adjust_for_add_for_test(const OpCtx &ctx, Ct a, Ct b);
Ct eval_mult_scalar_for_test(const OpCtx &ctx, const Ct &ct, double scalar);

// Cheby T-tree exposed for byte-parity testing vs OpenFHE
// cc->EvalChebyPolys's seriesPowers struct. Returns T, T2, T2km1 (k, m
// inferred from the input degree).
struct ChebyTreeForTest {
    std::vector<Ct> T;
    std::vector<Ct> T2;
    Ct T2km1;
    std::uint32_t k;
    std::uint32_t m;
};
ChebyTreeForTest compute_cheby_tree_for_test(const OpCtx &ctx, const Ct &x,
                                              const std::vector<double> &coeffs);

} // namespace haze::test::ops
