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
// TEST-ONLY reference oracle for the Montgomery / bit-reversal data format
// (Montgomery encoding, bit-reversed coefficient order, and the switchmodulus
// immediates). The production client ships NO Montgomery arithmetic — the
// replay driver owns the transform — so these helpers exist purely to let
// haze's data-format tests verify encodings and predict driver-substituted
// immediates without a compiler install. R is the Montgomery radix fixed by
// the executor; implemented from the standard formulations (Montgomery 1985).
//
// Keep this out of production code: the niobium::mod_arith namespace
// deliberately mirrors the compiler's proprietary src/ModArith.h naming for
// reviewer familiarity, and must never be included alongside it in one TU.

#pragma once

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace niobium::mod_arith {

/// (a * b) mod m for 64-bit operands via 128-bit intermediate.
inline uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
}

/// R mod q (R the Montgomery radix).
inline uint64_t montgomery_r(uint64_t q) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(1) << 64) % q);
}

/// Encode: x -> x * R mod q. Exact for any x (reduced first).
inline uint64_t to_montgomery(uint64_t x, uint64_t q) {
    return mulmod(x % q, montgomery_r(q), q);
}

/// a^e mod m by square-and-multiply.
inline uint64_t powmod(uint64_t a, uint64_t e, uint64_t m) {
    uint64_t r = 1U % m;
    a %= m;
    while (e > 0U) {
        if ((e & 1U) != 0U)
            r = mulmod(r, a, m);
        a = mulmod(a, a, m);
        e >>= 1U;
    }
    return r;
}

/// Modular inverse of a mod prime q (Fermat's little theorem).
inline uint64_t modinv_prime(uint64_t a, uint64_t q) {
    return powmod(a % q, q - 2U, q);
}

/// Decode: y -> y * R^{-1} mod q. q must be prime (RNS moduli are).
inline uint64_t from_montgomery(uint64_t y, uint64_t q) {
    return mulmod(y % q, modinv_prime(montgomery_r(q), q), q);
}

/// Bulk encode in place.
inline void to_montgomery(std::vector<uint64_t> &values, uint64_t q) {
    const uint64_t r = montgomery_r(q);
    for (auto &v : values)
        v = mulmod(v % q, r, q);
}

/// Bulk decode in place.
inline void from_montgomery(std::vector<uint64_t> &values, uint64_t q) {
    const uint64_t r_inv = modinv_prime(montgomery_r(q), q);
    for (auto &v : values)
        v = mulmod(v % q, r_inv, q);
}

/// Reverse the lowest `log2n` bits of `idx`.
inline uint64_t bit_reverse_index(uint64_t idx, unsigned log2n) {
    uint64_t out = 0;
    for (unsigned b = 0; b < log2n; ++b) {
        out = (out << 1) | ((idx >> b) & 1U);
    }
    return out;
}

/// Permute `values` into bit-reversed order in place. values.size() must be
/// a power of two. The permutation is an involution: applying it twice
/// restores the original order.
inline void apply_bit_reversal(std::vector<uint64_t> &values) {
    const uint64_t n = values.size();
    assert(n != 0 && (n & (n - 1)) == 0 && "ring dimension must be a power of two");
    unsigned log2n = 0;
    while ((1ULL << log2n) < n)
        ++log2n;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t j = bit_reverse_index(i, log2n);
        if (i < j)
            std::swap(values[i], values[j]);
    }
}

/// Immediates for the 4-instruction SwitchModulus sequence
/// (muli imm0 mod old_q / addi imm1 mod old_q / muli imm2 mod new_p /
/// addi imm3 mod new_p) that rebases a residue from old_q to new_p using the
/// centered (signed-preserving) representation:
///
///   Ordinary form:   [1, halfQ, 1, -halfQ mod p]
///   Montgomery form: [1, halfQ, R^2 mod p, -(R * halfQ) mod p]
///
/// with halfQ = (old_q - 1) / 2 and R the Montgomery radix. In Montgomery form the
/// muli-by-ordinary-1 REDCs the value out of Montgomery form, the centered
/// add happens in ordinary form, and the muli-by-R^2 rebases into new_p and
/// back into Montgomery form, so imm3 must be the Montgomery encoding of
/// -halfQ. Matches the FUNC_SIM lowering convention.
struct SwitchModulusImmediates {
    uint64_t imm[4];
};

inline SwitchModulusImmediates compute_switchmodulus_immediates(uint64_t old_q, uint64_t new_p,
                                                                bool montgomery) {
    SwitchModulusImmediates out{};
    const uint64_t half_q = (old_q - 1U) / 2U;
    out.imm[0] = 1U;
    out.imm[1] = half_q;
    if (!montgomery) {
        out.imm[2] = 1U;
        const uint64_t x = half_q % new_p;
        out.imm[3] = (x == 0U) ? 0U : new_p - x;
    } else {
        const uint64_t r_mod_p = montgomery_r(new_p);
        out.imm[2] = mulmod(r_mod_p, r_mod_p, new_p); // R^2 mod p
        const uint64_t r_half = mulmod(r_mod_p, half_q % new_p, new_p);
        out.imm[3] = (r_half == 0U) ? 0U : new_p - r_half;
    }
    return out;
}

} // namespace niobium::mod_arith
