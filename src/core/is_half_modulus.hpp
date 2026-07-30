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

#include "common/errors.hpp"
#include "common/handle.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace haze {

// Internal entry points for hazeIsHalfModulus / hazeIsHalfModulusMrp, the
// per-coefficient predicate dst_i = (src_i > q/2) ? 1 : 0; each opens an
// EpochSession internally.

std::expected<void, HazeInternalError> is_half_modulus(DevAddr dst, DevAddr src,
                                                       int mod_idx) noexcept;

std::expected<void, HazeInternalError> is_half_modulus_mrp(void *const *dst, const void *const *src,
                                                           const uint64_t *base,
                                                           std::size_t base_len) noexcept;

} // namespace haze
