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

#include <expected>
#include <haze/haze_types.h>

namespace haze {

// Device ciphertext-modulus envelope (reported as
// hazeDeviceProp::maxCiphertextModuli). Also the upper bound on an MRP
// group's residue count: a valid group cannot span more residues than the
// device supports moduli, so the C-ABI batch entry points reject a larger
// `count` rather than attempting an unbounded reservation.
inline constexpr int kMaxCiphertextModuli = 64;

// Supported ring-dimension envelope (reported as
// hazeDeviceProp::supportedRingDimExponents: N = 2^10 .. 2^16). Config
// validates hazeSetRingDimension against the same range so a dimension
// the device can't run — or one whose byte size would overflow
// n * sizeof(uint64_t) — is rejected at the setter.
inline constexpr int kMinRingDimExponent = 10;
inline constexpr int kMaxRingDimExponent = 16;

// Single-device runtime state. Only one device exists; the only valid
// device index is 0. Free functions instead of a class — a class adds
// nothing over a one-int counter and a couple of getters.

int device_count() noexcept;
int device_active() noexcept;
std::expected<void, HazeInternalError> device_set_active(int device) noexcept;
std::expected<void, HazeInternalError> device_fill_properties(hazeDeviceProp *prop,
                                                              int device) noexcept;

} // namespace haze
