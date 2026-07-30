// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
//
// hazeIsHalfModulus lowering. The FHETCH ISA has no comparison instruction; the
// predicate b = [x > q/2] is extracted arithmetically through an auxiliary prime
// p using two centered modulus switches (the only integer-level primitive that
// survives every replay mode — a bare cross-modulus rebase is not
// Montgomery-safe).

#include "core/is_half_modulus.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/config.hpp"
#include "core/device.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <numeric>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// a * b mod m via 128-bit product.
uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t m) noexcept {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
}

// Fermat inverse a^(p-2) mod p; requires p prime and a not divisible by p.
uint64_t modinv_prime(uint64_t a, uint64_t p) noexcept {
    uint64_t result = 1;
    uint64_t base = a % p;
    uint64_t exp = p - 2;
    while (exp > 0) {
        if ((exp & 1U) != 0)
            result = mulmod_u64(result, base, p);
        base = mulmod_u64(base, base, p);
        exp >>= 1U;
    }
    return result;
}

// Centered modulus switch q -> p: c(v) = v mod p for v <= (q-1)/2, else
// (v - q) mod p. Same lowering as fhetch's center_mod_q_into_p
// (shift / rebase / unshift; ordinary-form immediates; the imm=1 cross-modulus
// multiply is the SwitchModulus placeholder), with the montgomery config
// prepending the identity multiply so the chain forms the muli/addi/muli/addi
// quadruple the hardware replay driver substitutes — the same shape keying as
// fbc_center_shape() in basis_convert.cpp. Intermediates must stay single-use
// SSA values or the hardware substitution silently misses the chain.
fhetch::Polynomial emit_centered_switch(const fhetch::Polynomial &v, uint64_t q, uint64_t p) {
    const uint64_t half_q = (q - 1) / 2;
    const uint64_t half_mod_p = half_q % p;
    const uint64_t neg_half = (half_mod_p == 0) ? 0 : p - half_mod_p;
    const fhetch::Polynomial shift_in =
        replay_config().montgomery() ? fhetch::sr_mulps(v, fhetch::Scalar::from_int(1), q) : v;
    const auto shifted = fhetch::sr_addps(shift_in, fhetch::Scalar::from_int(half_q), q);
    const auto rebased = fhetch::sr_mulps(shifted, fhetch::Scalar::from_int(1), p);
    return fhetch::sr_addps(rebased, fhetch::Scalar::from_int(neg_half), p);
}

// b = [x > h] with h = (q-1)/2, extracted via two centered switches into p:
//   g1 = c(x - h mod q) == x - h        (signed value; x - h in [-h, h])
//   g2 = c(x)           == x - q*b      (mod p)
//   b  = ((g1 - g2 + h) mod p) * q^-1 == q*b * q^-1 in {0, 1}
// Exact for any prime p != q (mod-p arithmetic is exact ring arithmetic);
// immediates are pre-reduced mod their op's modulus because the replay
// backends assume scalar operands below the modulus.
fhetch::Polynomial lower_is_half_modulus(const fhetch::Polynomial &x, uint64_t q, uint64_t p) {
    const uint64_t h = (q - 1) / 2;
    const auto w = fhetch::sr_subps(x, fhetch::Scalar::from_int(h), q);
    const auto g1 = emit_centered_switch(w, q, p);
    const auto g2 = emit_centered_switch(x, q, p);
    fhetch::Polynomial qb = [&] {
        if (h % p != 0) {
            const auto diff = fhetch::sr_subp(g1, g2, p);
            return fhetch::sr_addps(diff, fhetch::Scalar::from_int(h % p), p);
        }
        // p | h degenerates every h-derived immediate to 0: the replay driver's
        // identity folds then elide the gadget unshifts and re-add, exposing
        // sub(muli(u,1,p), muli(v,1,p)) to its scalar_factor_sub rewrite, which
        // hoists the sub above the cross-modulus rebase onto unreduced ring-q
        // operands — wrong results on the transport path. A +1/-1 pair keeps a
        // non-elidable addps between the rebase and the sub.
        const auto barrier = fhetch::sr_addps(g1, fhetch::Scalar::from_int(1), p);
        const auto diff = fhetch::sr_subp(barrier, g2, p);
        return fhetch::sr_addps(diff, fhetch::Scalar::from_int(p - 1), p);
    }();
    return fhetch::sr_mulps(qb, fhetch::Scalar::from_int(modinv_prime(q, p)), p);
}

} // namespace

std::expected<void, HazeInternalError> is_half_modulus(DevAddr dst, DevAddr src,
                                                       int mod_idx) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    const uint64_t q = fhe_params().modulus(mod_idx);
    if (q == 0) {
        record_internal_error(HazeInternalError::InvalidArgument, "hazeIsHalfModulus: bad mod_idx");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Aux prime: lowest configured index != mod_idx (deterministic under the
    // sealed configuration; uniqueness of moduli guarantees p != q).
    const uint64_t p = fhe_params().modulus(mod_idx == 0 ? 1 : 0);
    if (p == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: no second configured modulus for the aux prime");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (std::gcd(q, p) != 1) {
        // Distinct primes are coprime; a composite chain voids the modulus contract.
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: modulus and aux modulus not coprime");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    auto x = epoch().lookup_or_create_locked(src);
    if (!x)
        return std::unexpected(x.error());

    epoch().store_compute_result_locked(dst, lower_is_half_modulus(*x, q, p), p);
    return {};
}

std::expected<void, HazeInternalError> is_half_modulus_mrp(void *const *dst, const void *const *src,
                                                           const uint64_t *base,
                                                           std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    // Shared aux prime, auto-selected like the SRP variant's: the lowest
    // configured index whose prime is not in base (deterministic under the
    // sealed configuration). One aux serves every limb.
    uint64_t aux_modulus = 0;
    for (int j = 0; j < kMaxCiphertextModuli; ++j) {
        const uint64_t candidate = fhe_params().modulus(j);
        if (candidate == 0)
            break; // configured moduli are contiguous from index 0
        bool in_base = false;
        for (std::size_t i = 0; i < base_len; ++i) {
            if (base[i] == candidate) {
                in_base = true;
                break;
            }
        }
        if (!in_base) {
            aux_modulus = candidate;
            break;
        }
    }
    if (aux_modulus == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulusMrp: no configured modulus outside base for the "
                              "aux prime");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    for (std::size_t i = 0; i < base_len; ++i) {
        // q^-1 mod aux must exist for every limb; distinct primes always
        // qualify, so this only rejects composite chains.
        if (std::gcd(base[i], aux_modulus) != 1) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeIsHalfModulusMrp: aux modulus not coprime to base prime");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    // Resolve every source before any store so a bad residue (null pointer,
    // never-written address) fails without half-mutating the dst array — the
    // build_mrp_locked / store_mrp_locked split the sibling MRP ops use.
    std::vector<fhetch::Polynomial> sources;
    sources.reserve(base_len);
    for (std::size_t i = 0; i < base_len; ++i) {
        auto x = epoch().lookup_or_create_locked(to_dev_addr(src[i]));
        if (!x)
            return std::unexpected(x.error());
        sources.push_back(std::move(*x));
    }
    // Per-limb fan-out: the predicate is a per-limb quantity, so each output is
    // an independent single-residue polynomial recorded under the shared aux
    // prime (a same-prime MRP grouping would alias residue keys).
    for (std::size_t i = 0; i < base_len; ++i) {
        epoch().store_compute_result_locked(to_dev_addr(dst[i]),
                                            lower_is_half_modulus(sources[i], base[i], aux_modulus),
                                            aux_modulus);
    }
    return {};
}

} // namespace haze
