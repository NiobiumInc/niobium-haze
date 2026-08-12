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
// TEST-ONLY modular-arithmetic oracle: exact 64-bit modmul/modpow/modinv for
// predicting recorded immediates and checking residues. Data-format arithmetic
// is absent by design — the replay driver owns any hardware transform.
//
// Keep this out of production code: the niobium::mod_arith namespace
// deliberately mirrors the compiler's proprietary src/ModArith.h naming for
// reviewer familiarity, and must never be included alongside it in one TU.

#pragma once

#include <cstdint>

namespace niobium::mod_arith {

/// (a * b) mod m for 64-bit operands via 128-bit intermediate.
inline uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
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

} // namespace niobium::mod_arith
