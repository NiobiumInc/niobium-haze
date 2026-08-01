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

#include "common/mod_arith.hpp"

#include <cstdint>

namespace haze {

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

uint64_t modinv_prime(uint64_t a, uint64_t p) noexcept {
    return powmod_u64(a, p - 2, p);
}

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

} // namespace haze
