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
#include <niobium/fhetch_api.h>

namespace haze {

// Centered modulus switch q -> p: c(v) = v mod p for v <= (q-1)/2, else
// (v - q) mod p. Emits the exact shape of fhetch's center_mod_q_into_p
// (vendor-internal; montgomery keying as basis_convert.cpp's fbc_center_shape)
// so the hardware replay driver recognizes and substitutes the chain;
// intermediates must stay single-use SSA values.
niobium::fhetch::Polynomial emit_centered_switch(const niobium::fhetch::Polynomial &v, uint64_t q,
                                                 uint64_t p);

} // namespace haze
