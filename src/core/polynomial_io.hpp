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
#include <vector>

namespace haze {

// Read a fhetch::Polynomial's integer values via int_data(); true on
// success, false for an invalid/non-integer/empty polynomial, never throws.
bool decode_result_values(const niobium::fhetch::Polynomial &p,
                          std::vector<uint64_t> &out) noexcept;

} // namespace haze
