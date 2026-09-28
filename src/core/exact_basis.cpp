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

#include "core/exact_basis.hpp"

#include "core/centered_switch.hpp"

#include <cstdint>
#include <niobium/fhetch_api.h>
#include <span>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::vector<fhetch::Polynomial> raw_lift(const fhetch::Polynomial &v, uint64_t q,
                                         std::span<const uint64_t> dst_base) noexcept {
    const uint64_t half_q = (q - 1) / 2;
    // One shift feeds every destination's chain; it is the chain's input, not
    // one of the intermediates the driver's SwitchModulus match needs single-use.
    const auto shifted = fhetch::sr_subps(v, fhetch::Scalar::from_int(half_q), q);
    std::vector<fhetch::Polynomial> lifted;
    lifted.reserve(dst_base.size());
    for (const uint64_t p : dst_base) {
        const auto centered = emit_centered_switch(shifted, q, p);
        lifted.push_back(fhetch::sr_addps(centered, fhetch::Scalar::from_int(half_q % p), p));
    }
    return lifted;
}

} // namespace haze
