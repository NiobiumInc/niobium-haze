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
#include <functional>

namespace haze {

// Virtual HBM base for HAZE's DeviceAllocator; the 256 GiB offset sits above any
// plausible upstream synthetic-address range so HAZE DevAddr values cannot collide
// with synthetic addresses fhetch emits in the recorded IR.
inline constexpr uintptr_t kHbmBase = 0x4000000000ULL;

// Strong-typed device address wrapping the uintptr_t cast from the `void*` handed to
// the user; internal code works with DevAddr and `void*` lives only at the C ABI
// boundary.
enum class DevAddr : uintptr_t {};

inline DevAddr to_dev_addr(const void *p) noexcept {
    return DevAddr{reinterpret_cast<uintptr_t>(p)};
}

inline uintptr_t to_uintptr(DevAddr a) noexcept {
    return static_cast<uintptr_t>(a);
}

inline void *to_void_ptr(DevAddr a) noexcept {
    // The cast back to void* at the C ABI boundary is unavoidable, and the
    // int-to-ptr performance hint does not apply to handle types.
    return reinterpret_cast<void *>( // NOLINT(performance-no-int-to-ptr)
        static_cast<uintptr_t>(a));
}

} // namespace haze

namespace std {
template <> struct hash<haze::DevAddr> {
    size_t operator()(haze::DevAddr a) const noexcept {
        return hash<uintptr_t>{}(haze::to_uintptr(a));
    }
};
} // namespace std
