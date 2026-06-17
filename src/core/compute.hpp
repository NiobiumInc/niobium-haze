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
#include "core/config.hpp"
#include "core/context.hpp"
#include "core/graph.hpp"
#include "core/lower.hpp"
#include "core/mrp_polymap.hpp"
#include "core/record.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

namespace haze {

// Each compute prelude freezes the parameters, resolves the operands to
// tape values, binds the destination, and appends one Node whose thunk
// dispatches the FHETCH op at flush time. Operands are resolved before
// the dst bind so in-place ops (dst == src1 [== src2]) read the old
// value.
//
// THUNKS CAPTURE PLAIN VALUES ONLY (ValueId / uint64_t / vectors) — see
// the discipline note in core/graph.hpp. fhetch objects (Scalar, MRS,
// ModuliBase, Polynomial) are constructed inside the thunk at lowering.

// Polynomial-polynomial-modulus. Used by hazeAdd, hazeSub, hazeMul.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_pp_op(Context &ctx, DevAddr dst, DevAddr src1,
                                                    DevAddr src2, int mod_idx) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "compute on a context with no parameters");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    const uint64_t q = cfg->modulus(mod_idx);
    if (q == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "mod_idx names no configured modulus");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    const auto v1 = resolve_operand(ctx, src1, cfg->ring_dim);
    if (!v1)
        return std::unexpected(v1.error());
    const auto v2 = resolve_operand(ctx, src2, cfg->ring_dim);
    if (!v2)
        return std::unexpected(v2.error());
    const ValueId d = bind_result(ctx, dst, q);

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dst;
    node.dst_vid = d;
    node.src_vids = {*v1, *v2};
    node.entry = "haze binary pp op";
    node.thunk = [a = *v1, b = *v2, d,
                  q](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        const auto p1 = lower.poly(a);
        if (!p1)
            return std::unexpected(p1.error());
        const auto p2 = lower.poly(b);
        if (!p2)
            return std::unexpected(p2.error());
        lower.bind(d, OpFn(**p1, **p2, q));
        return {};
    };
    ctx.tape.append(std::move(node));
    return {};
}

// Polynomial-scalar-modulus. Used by hazeAddScalar, hazeSubScalar, hazeMulScalar.
template <auto OpFn>
std::expected<void, HazeInternalError> binary_ps_op(Context &ctx, DevAddr dst, DevAddr src,
                                                    uint64_t scalar, int mod_idx) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "compute on a context with no parameters");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    const uint64_t q = cfg->modulus(mod_idx);
    if (q == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "mod_idx names no configured modulus");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    const auto v = resolve_operand(ctx, src, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());
    const ValueId d = bind_result(ctx, dst, q);

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dst;
    node.dst_vid = d;
    node.src_vids = {*v};
    node.entry = "haze binary ps op";
    node.thunk = [s = *v, d, scalar, q](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        const auto p = lower.poly(s);
        if (!p)
            return std::unexpected(p.error());
        lower.bind(d, OpFn(**p, niobium::fhetch::Scalar::from_int(scalar), q));
        return {};
    };
    ctx.tape.append(std::move(node));
    return {};
}

// Polynomial-modulus. Used by hazeNTT, hazeINTT.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pq_op(Context &ctx, DevAddr dst, DevAddr src,
                                                   int mod_idx) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "compute on a context with no parameters");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    const uint64_t q = cfg->modulus(mod_idx);
    if (q == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "mod_idx names no configured modulus");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    const auto v = resolve_operand(ctx, src, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());
    const ValueId d = bind_result(ctx, dst, q);

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dst;
    node.dst_vid = d;
    node.src_vids = {*v};
    node.entry = "haze unary pq op";
    node.thunk = [s = *v, d, q](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        const auto p = lower.poly(s);
        if (!p)
            return std::unexpected(p.error());
        lower.bind(d, OpFn(**p, q));
        return {};
    };
    ctx.tape.append(std::move(node));
    return {};
}

// Polynomial-index (hazeAutomorph). The eval-form automorph is a pure
// slot permutation, so the op carries the COPY sentinel; recover the
// source's recorded modulus and bind it (source + result) so a tagged
// SRP automorph is probe-serializable on transport.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_pi_op(Context &ctx, DevAddr dst, DevAddr src,
                                                   uint64_t index) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);
    const auto v = resolve_operand(ctx, src, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());
    const uint64_t q = recorded_modulus(ctx, src);
    const ValueId d = bind_result(ctx, dst, q);

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dst;
    node.dst_vid = d;
    node.src_vids = {*v};
    node.entry = "haze unary pi op";
    node.thunk = [s = *v, d, index, q](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        const auto p = lower.poly(s);
        if (!p)
            return std::unexpected(p.error());
        auto result = OpFn(**p, index);
        if (q != kCopyModulus) {
            niobium::fhetch::bind_modulus(**p, q);
            niobium::fhetch::bind_modulus(result, q);
        }
        lower.bind(d, std::move(result));
        return {};
    };
    ctx.tape.append(std::move(node));
    return {};
}

// MRP compute templates: same shape, fanned out per residue via
// record_mrp_sources / record_mrp_dests. In-place safety carries over
// per-residue (all sources resolved before any dst bind).

namespace detail {

// Shared tail for the MRP templates: bind the dst residues, append the
// compute node, register the output group.
template <typename ThunkFactory>
std::expected<void, HazeInternalError>
append_mrp_compute(Context &ctx, void *const *dst, const uint64_t *base, std::size_t base_len,
                   std::vector<ValueId> &&src_vids, const char *entry, ThunkFactory &&factory) {
    MrpDests dests = record_mrp_dests(ctx, dst, base, base_len);
    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dests.addrs.front();
    node.group_addrs = dests.addrs;
    node.group_vids = dests.vids;
    node.src_vids = std::move(src_vids);
    node.entry = entry;
    node.thunk = std::forward<ThunkFactory>(factory)(dests.vids);
    ctx.tape.append(std::move(node));
    return record_mrp_out_group(ctx, dests.addrs, base, base_len);
}

} // namespace detail

// MRP polynomial-polynomial. Used by hazeAddMrp, hazeSubMrp, hazeMulMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_pp_op_mrp(Context &ctx, void *const *dst, const void *const *src1, const void *const *src2,
                 const uint64_t *base, std::size_t base_len) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);
    auto v1 = record_mrp_sources(ctx, src1, base, base_len, cfg->ring_dim);
    if (!v1)
        return std::unexpected(v1.error());
    auto v2 = record_mrp_sources(ctx, src2, base, base_len, cfg->ring_dim);
    if (!v2)
        return std::unexpected(v2.error());

    std::vector<ValueId> srcs = *v1;
    srcs.insert(srcs.end(), v2->begin(), v2->end());
    const std::vector<uint64_t> base_vec(base, base + base_len);
    return detail::append_mrp_compute(
        ctx, dst, base, base_len, std::move(srcs), "haze mrp pp op",
        [&](const std::vector<ValueId> &dv) {
            return [a = std::move(*v1), b = std::move(*v2), dv,
                    base_vec](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
                auto m1 = build_lowered_mrp(lower, a, base_vec);
                if (!m1)
                    return std::unexpected(m1.error());
                auto m2 = build_lowered_mrp(lower, b, base_vec);
                if (!m2)
                    return std::unexpected(m2.error());
                const niobium::fhetch::MRP result = OpFn(*m1, *m2);
                for (std::size_t i = 0; i < dv.size(); ++i)
                    lower.bind(dv[i], result[base_vec[i]]);
                return {};
            };
        });
}

// MRP polynomial-scalar (scalars[i] pairs with base[i]); used by
// hazeAddScalarMrp, hazeSubScalarMrp, hazeMulScalarMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
binary_ps_op_mrp(Context &ctx, void *const *dst, const void *const *src, const uint64_t *scalars,
                 const uint64_t *base, std::size_t base_len) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);
    auto v = record_mrp_sources(ctx, src, base, base_len, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());

    std::vector<ValueId> srcs = *v;
    const std::vector<uint64_t> base_vec(base, base + base_len);
    std::vector<uint64_t> scalar_vec(scalars, scalars + base_len);
    return detail::append_mrp_compute(
        ctx, dst, base, base_len, std::move(srcs), "haze mrp ps op",
        [&](const std::vector<ValueId> &dv) {
            return [s = std::move(*v), dv, base_vec, scalar_vec = std::move(scalar_vec)](
                       LowerCtx &lower) -> std::expected<void, HazeInternalError> {
                auto m = build_lowered_mrp(lower, s, base_vec);
                if (!m)
                    return std::unexpected(m.error());
                const niobium::fhetch::MRP result =
                    OpFn(*m, build_mrs(scalar_vec.data(), base_vec.data(), base_vec.size()));
                for (std::size_t i = 0; i < dv.size(); ++i)
                    lower.bind(dv[i], result[base_vec[i]]);
                return {};
            };
        });
}

// MRP polynomial-only: mr_ntt/mr_intt carry their moduli base inside the
// MRP. Used by hazeNTTMrp, hazeINTTMrp.
template <auto OpFn>
std::expected<void, HazeInternalError> unary_p_op_mrp(Context &ctx, void *const *dst,
                                                      const void *const *src, const uint64_t *base,
                                                      std::size_t base_len) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);
    auto v = record_mrp_sources(ctx, src, base, base_len, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());

    std::vector<ValueId> srcs = *v;
    const std::vector<uint64_t> base_vec(base, base + base_len);
    return detail::append_mrp_compute(
        ctx, dst, base, base_len, std::move(srcs), "haze mrp unary op",
        [&](const std::vector<ValueId> &dv) {
            return [s = std::move(*v), dv,
                    base_vec](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
                auto m = build_lowered_mrp(lower, s, base_vec);
                if (!m)
                    return std::unexpected(m.error());
                const niobium::fhetch::MRP result = OpFn(*m);
                for (std::size_t i = 0; i < dv.size(); ++i)
                    lower.bind(dv[i], result[base_vec[i]]);
                return {};
            };
        });
}

// MRP polynomial-with-index. Used by hazeAutomorphMrp (mr_automorph_eval
// takes an odd integer k in [1, 2N-1]) and hazeRotAutomorphCoeffMrp.
template <auto OpFn>
std::expected<void, HazeInternalError>
unary_pi_op_mrp(Context &ctx, void *const *dst, const void *const *src, uint64_t index,
                const uint64_t *base, std::size_t base_len) noexcept {
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);
    auto v = record_mrp_sources(ctx, src, base, base_len, cfg->ring_dim);
    if (!v)
        return std::unexpected(v.error());

    std::vector<ValueId> srcs = *v;
    const std::vector<uint64_t> base_vec(base, base + base_len);
    return detail::append_mrp_compute(
        ctx, dst, base, base_len, std::move(srcs), "haze mrp pi op",
        [&](const std::vector<ValueId> &dv) {
            return [s = std::move(*v), dv, base_vec,
                    index](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
                auto m = build_lowered_mrp(lower, s, base_vec);
                if (!m)
                    return std::unexpected(m.error());
                const niobium::fhetch::MRP result = OpFn(*m, index);
                for (std::size_t i = 0; i < dv.size(); ++i)
                    lower.bind(dv[i], result[base_vec[i]]);
                return {};
            };
        });
}

} // namespace haze
