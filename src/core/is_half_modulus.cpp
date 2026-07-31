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
// hazeIsHalfModulus lowering: the FHETCH ISA has no comparison instruction, so
// the predicate is extracted arithmetically via two centered modulus switches
// through an auxiliary prime.

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

uint64_t powmod_u64(uint64_t base, uint64_t exp, uint64_t m) noexcept {
    uint64_t result = 1;
    base %= m;
    while (exp > 0) {
        if ((exp & 1U) != 0)
            result = mulmod_u64(result, base, m);
        base = mulmod_u64(base, base, m);
        exp >>= 1U;
    }
    return result;
}

// Fermat inverse a^(p-2) mod p; requires p prime and a not divisible by p.
uint64_t modinv_prime(uint64_t a, uint64_t p) noexcept {
    return powmod_u64(a, p - 2, p);
}

// Deterministic Miller-Rabin for 64-bit n (the 12-base set is exact below 3.3e24).
// Guards the aux selection: a composite aux would make the Fermat inverse silently
// wrong.
bool is_prime_u64(uint64_t n) noexcept {
    constexpr uint64_t kBases[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2)
        return false;
    for (uint64_t b : kBases) {
        if (n % b == 0)
            return n == b;
    }
    uint64_t d = n - 1;
    unsigned s = 0;
    while (d % 2 == 0) {
        d /= 2;
        ++s;
    }
    for (uint64_t b : kBases) {
        uint64_t x = powmod_u64(b, d, n);
        if (x == 1 || x == n - 1)
            continue;
        bool composite = true;
        for (unsigned i = 1; i < s; ++i) {
            x = mulmod_u64(x, x, n);
            if (x == n - 1) {
                composite = false;
                break;
            }
        }
        if (composite)
            return false;
    }
    return true;
}

// Centered modulus switch q -> p: c(v) = v mod p for v <= (q-1)/2, else
// (v - q) mod p. Emits the exact shape of fhetch's center_mod_q_into_p
// (vendor-internal; montgomery keying as basis_convert.cpp's fbc_center_shape)
// so the hardware replay driver recognizes and substitutes the chain;
// intermediates must stay single-use SSA values.
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

// b = [x > h], h = (q-1)/2: g1 = c(x-h) == x-h (signed), g2 = c(x) == x - q*b,
// so ((g1 - g2 + h) mod p) * q^-1 == b exactly, for any prime p != q.
// Immediates are pre-reduced (replay backends assume scalars below the modulus).
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
        // p | h zeroes every h-derived immediate; the replay driver's identity
        // folds then expose sub(muli,muli) to scalar_factor_sub, which hoists
        // the sub above the rebase onto unreduced ring-q operands. The +1/-1
        // pair keeps a non-elidable addps in between.
        // TODO(niobium-compiler): drop once scalar_factor_sub gains a
        // cross-ring guard; deployed drivers predate that fix.
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
    if (q % 2 == 0) {
        // h = (q-1)/2 centering needs an odd modulus.
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: modulus must be odd");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Aux prime: lowest configured index != mod_idx holding a prime; composite
    // entries are skipped (the Fermat inverse below is only an inverse mod a
    // prime — a composite aux would yield silently wrong results).
    uint64_t p = 0;
    for (int j = 0; j < kMaxCiphertextModuli; ++j) {
        if (j == mod_idx)
            continue;
        const uint64_t candidate = fhe_params().modulus(j);
        if (candidate == 0)
            break; // configured moduli are contiguous from index 0
        if (is_prime_u64(candidate)) {
            p = candidate;
            break;
        }
    }
    if (p == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeIsHalfModulus: no prime configured modulus for the aux");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (std::gcd(q, p) != 1) {
        // q^-1 mod p must exist; p is prime, so this only rejects p | q.
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
    // Shared aux prime: lowest configured index whose value is prime and not in
    // base; composite entries are skipped (a composite aux would make the
    // Fermat inverse silently wrong).
    uint64_t aux_modulus = 0;
    for (int j = 0; j < kMaxCiphertextModuli; ++j) {
        const uint64_t candidate = fhe_params().modulus(j);
        if (candidate == 0)
            break; // configured moduli are contiguous from index 0
        if (!is_prime_u64(candidate))
            continue;
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
                              "hazeIsHalfModulusMrp: no prime configured modulus outside base for "
                              "the aux");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    for (std::size_t i = 0; i < base_len; ++i) {
        if (base[i] % 2 == 0) {
            // h = (q_i-1)/2 centering needs odd limb moduli.
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeIsHalfModulusMrp: base modulus must be odd");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        // q_i^-1 mod aux must exist; aux is prime, so this only rejects aux | q_i.
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
    // Resolve every source before any store so a bad residue fails without
    // half-mutating the dst array (the sibling ops' build/store split).
    std::vector<fhetch::Polynomial> sources;
    sources.reserve(base_len);
    for (std::size_t i = 0; i < base_len; ++i) {
        auto x = epoch().lookup_or_create_locked(to_dev_addr(src[i]));
        if (!x)
            return std::unexpected(x.error());
        sources.push_back(std::move(*x));
    }
    // Outputs stay independent single-residue polynomials: a same-prime MRP
    // grouping would alias residue keys.
    for (std::size_t i = 0; i < base_len; ++i) {
        epoch().store_compute_result_locked(to_dev_addr(dst[i]),
                                            lower_is_half_modulus(sources[i], base[i], aux_modulus),
                                            aux_modulus);
    }
    return {};
}

} // namespace haze
