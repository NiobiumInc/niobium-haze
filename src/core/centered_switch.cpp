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

#include "core/centered_switch.hpp"

#include "core/config.hpp"

#include <cstdint>
#include <niobium/fhetch_api.h>

namespace haze {

namespace fhetch = niobium::fhetch;

niobium::fhetch::Polynomial emit_centered_switch(const niobium::fhetch::Polynomial &v, uint64_t q,
                                                 uint64_t p) {
    const uint64_t half_q = (q - 1) / 2;
    const uint64_t half_mod_p = half_q % p;
    const uint64_t neg_half = (half_mod_p == 0) ? 0 : p - half_mod_p;
    const fhetch::Polynomial shift_in =
        replay_config().montgomery() ? fhetch::sr_mulps(v, fhetch::Scalar::from_int(1), q) : v;
    const auto shifted = fhetch::sr_addps(shift_in, fhetch::Scalar::from_int(half_q), q);
    const auto rebased = fhetch::sr_mulps(shifted, fhetch::Scalar::from_int(1), p);
    return fhetch::sr_addps(rebased, fhetch::Scalar::from_int(neg_half), p);
}

} // namespace haze
