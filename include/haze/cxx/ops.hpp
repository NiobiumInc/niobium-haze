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
//
// Thin typed wrappers over the compute C ABI. Each validates that the
// operands' bases agree BEFORE the call — the class of mismatch the raw
// void**/base_len ABI can only catch at runtime depth (or not at all)
// fails here at the boundary with HAZE_ERROR_INVALID_VALUE. Moduli are
// always taken from the handles, never from ambient config state.
#pragma once

#include "haze/cxx/error.hpp"
#include "haze/cxx/handles.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <span>

namespace haze::cxx::inline v1 {

namespace detail {

inline bool same_base(const Mrp &a, const Mrp &b) noexcept {
    return a.base_len() == b.base_len() && std::equal(a.base(), a.base() + a.base_len(), b.base());
}

} // namespace detail

// ---- Single-residue ops (modulus from the handle's ModSlot) ----

inline Status add(Srp &dst, const Srp &a, const Srp &b) noexcept {
    if (a.mod() != b.mod() || a.mod() != dst.mod())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeAdd(dst.context(), dst.addr(), a.addr(), b.addr(), dst.mod().index, nullptr)};
}

inline Status sub(Srp &dst, const Srp &a, const Srp &b) noexcept {
    if (a.mod() != b.mod() || a.mod() != dst.mod())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeSub(dst.context(), dst.addr(), a.addr(), b.addr(), dst.mod().index, nullptr)};
}

inline Status mul(Srp &dst, const Srp &a, const Srp &b) noexcept {
    if (a.mod() != b.mod() || a.mod() != dst.mod())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeMul(dst.context(), dst.addr(), a.addr(), b.addr(), dst.mod().index, nullptr)};
}

// ---- Multi-residue ops (base agreement enforced across all operands) ----

inline Status add(Mrp &dst, const Mrp &a, const Mrp &b) noexcept {
    if (!detail::same_base(dst, a) || !detail::same_base(dst, b))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeAddMrp(dst.context(), dst.data(), a.data(), b.data(), dst.base(),
                             dst.base_len(), nullptr)};
}

inline Status sub(Mrp &dst, const Mrp &a, const Mrp &b) noexcept {
    if (!detail::same_base(dst, a) || !detail::same_base(dst, b))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeSubMrp(dst.context(), dst.data(), a.data(), b.data(), dst.base(),
                             dst.base_len(), nullptr)};
}

inline Status mul(Mrp &dst, const Mrp &a, const Mrp &b) noexcept {
    if (!detail::same_base(dst, a) || !detail::same_base(dst, b))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeMulMrp(dst.context(), dst.data(), a.data(), b.data(), dst.base(),
                             dst.base_len(), nullptr)};
}

// scalars[i] pairs with base[i].
inline Status add_scalar(Mrp &dst, const Mrp &a, std::span<const uint64_t> scalars) noexcept {
    if (!detail::same_base(dst, a) || scalars.size() != dst.base_len())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeAddScalarMrp(dst.context(), dst.data(), a.data(), scalars.data(), dst.base(),
                                   dst.base_len(), nullptr)};
}

inline Status sub_scalar(Mrp &dst, const Mrp &a, std::span<const uint64_t> scalars) noexcept {
    if (!detail::same_base(dst, a) || scalars.size() != dst.base_len())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeSubScalarMrp(dst.context(), dst.data(), a.data(), scalars.data(), dst.base(),
                                   dst.base_len(), nullptr)};
}

inline Status mul_scalar(Mrp &dst, const Mrp &a, std::span<const uint64_t> scalars) noexcept {
    if (!detail::same_base(dst, a) || scalars.size() != dst.base_len())
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeMulScalarMrp(dst.context(), dst.data(), a.data(), scalars.data(), dst.base(),
                                   dst.base_len(), nullptr)};
}

inline Status ntt(Mrp &dst, const Mrp &a) noexcept {
    if (!detail::same_base(dst, a))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{
        hazeNTTMrp(dst.context(), dst.data(), a.data(), dst.base(), dst.base_len(), nullptr)};
}

inline Status intt(Mrp &dst, const Mrp &a) noexcept {
    if (!detail::same_base(dst, a))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{
        hazeINTTMrp(dst.context(), dst.data(), a.data(), dst.base(), dst.base_len(), nullptr)};
}

inline Status automorph(Mrp &dst, const Mrp &a, uint64_t index) noexcept {
    if (!detail::same_base(dst, a))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeAutomorphMrp(dst.context(), dst.data(), a.data(), index, dst.base(),
                                   dst.base_len(), nullptr)};
}

inline Status rot_automorph_coeff(Mrp &dst, const Mrp &a, uint64_t offset) noexcept {
    if (!detail::same_base(dst, a))
        return Status{HAZE_ERROR_INVALID_VALUE};
    return Status{hazeRotAutomorphCoeffMrp(dst.context(), dst.data(), a.data(), offset, dst.base(),
                                           dst.base_len(), nullptr)};
}

// ---- Recording boundary ----

inline Status tag_output(const Srp &srp) noexcept {
    return Status{hazeTagOutput(srp.context(), srp.addr())};
}

// Tags every residue (matches the hand-written test harness; tagging
// any one residue would expand to the registered group anyway).
inline Status tag_output(const Mrp &mrp) noexcept {
    for (size_t i = 0; i < mrp.base_len(); ++i)
        if (const hazeError_t err = hazeTagOutput(mrp.context(), mrp.residue(i));
            err != HAZE_SUCCESS)
            return Status{err};
    return Status{};
}

// The graph boundary stays explicit: kernels record, flush() runs.
inline Status flush(hazeContext_t ctx) noexcept {
    return Status{hazeFlush(ctx)};
}

} // namespace haze::cxx::inline v1
