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
#include "core/allocator.hpp"
#include "core/config.hpp"
#include "core/epoch.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <optional>

namespace haze {

// Each compute prelude validates the destination, opens an EpochSession,
// confirms the recording started (a failed backend init surfaces here),
// resolves the modulus, copies sources from the polymap, dispatches the
// FHETCH op, and stores the result; sources are copied by value so
// in-place ops (dst == src1 [== src2]) stay correct.

// Polynomial-polynomial-modulus. Used by hazeAdd, hazeSub, hazeMul.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_pp_op(DevAddr dst, DevAddr src1, DevAddr src2,
                                                    int mod_idx) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    const uint64_t q = fhe_params().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p1 = epoch().lookup_or_create_locked(src1);
    if (!p1)
        return std::unexpected(p1.error());
    auto p2 = epoch().lookup_or_create_locked(src2);
    if (!p2)
        return std::unexpected(p2.error());
    epoch().store_compute_result_locked(dst, OpFn(*p1, *p2, q), q);
    return {};
}

// Polynomial-scalar-modulus. Used by hazeAddScalar, hazeSubScalar, hazeMulScalar.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_ps_op(DevAddr dst, DevAddr src, uint64_t scalar,
                                                    int mod_idx) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    const uint64_t q = fhe_params().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    epoch().store_compute_result_locked(dst, OpFn(*p, niobium::fhetch::Scalar::from_int(scalar), q),
                                        q);
    return {};
}

// NTT / INTT emission for one residue: q's caller root (FheParams::ntt_root)
// through the rooted form, else the rootless form, which records no root and
// leaves the minimal root to the trace consumer. hazeConfigureDevice validated
// every caller root for the ring dimension all polynomials share, so the rooted
// forms never reject.
template <auto Rootless, auto Rooted>
niobium::fhetch::Polynomial with_configured_root(const niobium::fhetch::Polynomial &a, uint64_t q) {
    const std::optional<uint64_t> root = fhe_params().ntt_root(q);
    return root.has_value() ? Rooted(a, q, *root) : Rootless(a, q);
}

// The MRP form applies the SRP one to each residue, as mr_ntt and
// mr_ntt_with_roots do, so a base may mix rooted primes with rootless ones (the
// aux prime, or a prime outside the chain).
template <auto SrpTransform> niobium::fhetch::MRP each_residue(const niobium::fhetch::MRP &x) {
    const niobium::fhetch::ModuliBase &base = x.base();
    niobium::fhetch::MRP z(base, x[base[0]].ring_dimension());
    for (const uint64_t q : base)
        z[q] = SrpTransform(x[q], q);
    return z;
}

inline constexpr auto record_ntt =
    &with_configured_root<niobium::fhetch::sr_ntt, niobium::fhetch::sr_ntt_with_root>;
inline constexpr auto record_intt =
    &with_configured_root<niobium::fhetch::sr_intt, niobium::fhetch::sr_intt_with_root>;
inline constexpr auto record_mr_ntt = &each_residue<record_ntt>;
inline constexpr auto record_mr_intt = &each_residue<record_intt>;

// Polynomial-modulus. Used by hazeNTT, hazeINTT (with record_ntt / record_intt).
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pq_op(DevAddr dst, DevAddr src, int mod_idx) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    const uint64_t q = fhe_params().modulus(mod_idx);
    if (q == 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    epoch().store_compute_result_locked(dst, OpFn(*p, q), q);
    return {};
}

// Polynomial-index (hazeAutomorph): the eval-form automorph is a pure slot
// permutation carrying the COPY sentinel, so bind the source's recorded modulus
// to source and result to keep the tagged SRP automorph probe-serializable.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pi_op(DevAddr dst, DevAddr src,
                                                   uint64_t index) noexcept {
    if (auto live = allocator().require_allocated(dst); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    auto p = epoch().lookup_or_create_locked(src);
    if (!p)
        return std::unexpected(p.error());
    const uint64_t q = epoch().recorded_modulus_locked(src);
    auto result = OpFn(*p, index);
    if (q != kCopyModulus) {
        niobium::fhetch::bind_modulus(*p, q);
        niobium::fhetch::bind_modulus(result, q);
    }
    epoch().store_compute_result_locked(dst, std::move(result), q);
    return {};
}

// MRP compute templates: same shape as SRP, fanned out per residue via
// build_mrp_locked / store_mrp_locked; in-place safety carries over per-residue.

// MRP polynomial-polynomial. Used by hazeAddMrp, hazeSubMrp, hazeMulMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_pp_op_mrp(void *const *dst, const void *const *src1, const void *const *src2,
                 const uint64_t *base, std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    auto m1 = build_mrp_locked(src1, base, base_len);
    if (!m1)
        return std::unexpected(m1.error());
    auto m2 = build_mrp_locked(src2, base, base_len);
    if (!m2)
        return std::unexpected(m2.error());
    niobium::fhetch::MRP result = OpFn(*m1, *m2);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-scalar (scalars[i] pairs with base[i]); used by
// hazeAddScalarMrp, hazeSubScalarMrp, hazeMulScalarMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_ps_op_mrp(void *const *dst, const void *const *src, const uint64_t *scalars,
                 const uint64_t *base, std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m, build_mrs(scalars, base, base_len));
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-only (hazeNTTMrp, hazeINTTMrp, with record_mr_ntt /
// record_mr_intt): the transform reads its base from the MRP, so this can't
// reuse unary_pq_op.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_p_op_mrp(void *const *dst, const void *const *src,
                                                      const uint64_t *base,
                                                      std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

// MRP polynomial-with-index. Used by hazeAutomorphMrp (mr_automorph_eval
// takes an odd integer k in [1, 2N-1] and is otherwise modulus-independent).
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pi_op_mrp(void *const *dst, const void *const *src,
                                                       uint64_t index, const uint64_t *base,
                                                       std::size_t base_len) noexcept {
    if (auto v = validate_moduli_base(base, base_len); !v)
        return v;
    if (auto live = require_allocated_array(dst, base_len); !live)
        return live;
    EpochSession session;
    if (auto rec = epoch().require_recording_locked(); !rec)
        return rec;
    auto m = build_mrp_locked(src, base, base_len);
    if (!m)
        return std::unexpected(m.error());
    niobium::fhetch::MRP result = OpFn(*m, index);
    auto stored = store_mrp_locked(dst, result, base, base_len);
    if (!stored)
        return std::unexpected(stored.error());
    return {};
}

} // namespace haze
