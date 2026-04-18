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

#include <haze/haze_types.h>

namespace haze::detail {

// Single-device runtime state. The active-device selector is the only
// mutable field; everything else (count, properties) is constexpr in
// the impl. Free functions instead of a class — there is one int of
// state and one invariant ("active must be 0"); a class adds nothing
// over an `int` plus a setter.

int device_count() noexcept;
int device_active() noexcept;
hazeError_t device_set_active(int device) noexcept;
hazeError_t device_fill_properties(hazeDeviceProp *prop, int device) noexcept;
void device_reset() noexcept;

} // namespace haze::detail
