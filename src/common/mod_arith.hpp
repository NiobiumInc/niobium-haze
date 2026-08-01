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
#pragma once

#include <cstdint>

namespace haze {

// Host-side scalar modular arithmetic (record-time immediates only, never a
// polynomial hot path).

// a * b mod m via 128-bit product.
uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t m) noexcept;

uint64_t powmod_u64(uint64_t base, uint64_t exp, uint64_t m) noexcept;

// Fermat inverse a^(p-2) mod p; requires p prime and a not divisible by p.
uint64_t modinv_prime(uint64_t a, uint64_t p) noexcept;

// Deterministic Miller-Rabin for 64-bit n (the 12-base set is exact below 3.3e24).
bool is_prime_u64(uint64_t n) noexcept;

} // namespace haze
