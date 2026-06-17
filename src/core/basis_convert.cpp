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

#include "core/basis_convert.hpp"

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
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// Validation helpers; each returns InvalidArgument with a debug-log
// breadcrumb on the first failure, keeping the C ABI shim thin.

std::expected<void, HazeInternalError> validate(const hazeBasisConvertParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.dst_base == nullptr ||
        p.dst_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBasisConvert: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModDownParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.rescale_base == nullptr ||
        p.rescale_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // rescale_base must be a *proper* subset of src_base — equal-length
    // would leave dst empty (upstream fhetch asserts the same).
    if (p.rescale_base_len >= p.src_base_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: rescale_base_len >= src_base_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Foreign-modulus check: every prime in rescale_base must appear in
    // src_base. Doing this HAZE-side rejects bad calls before fhetch's
    // assert (which strips in release).
    std::unordered_set<uint64_t> src_set(p.src_base, p.src_base + p.src_base_len);
    for (size_t j = 0; j < p.rescale_base_len; ++j) {
        if (!src_set.contains(p.rescale_base[j])) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeModDown: rescale_base not subset of src_base");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModUpParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.digit_bases == nullptr ||
        p.digit_base_lens == nullptr || p.digit_count == 0 || p.p_base == nullptr ||
        p.p_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument, "hazeModUp: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // digit_bases is a flat concatenation; the per-digit lengths must sum
    // to digit_bases_total_len. Catch caller miscounts before slicing
    // out-of-bounds.
    size_t sum = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        sum += p.digit_base_lens[i];
    }
    if (sum != p.digit_bases_total_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: digit_base_lens do not sum to digit_bases_total_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

} // namespace

std::expected<void, HazeInternalError> basis_convert(Context &ctx, void *const *dst,
                                                     const void *const *src,
                                                     const hazeBasisConvertParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);

    auto svids = record_mrp_sources(ctx, src, p.src_base, p.src_base_len, cfg->ring_dim);
    if (!svids)
        return std::unexpected(svids.error());

    const std::vector<uint64_t> src_base(p.src_base, p.src_base + p.src_base_len);
    const std::vector<uint64_t> dst_base(p.dst_base, p.dst_base + p.dst_base_len);
    MrpDests dests = record_mrp_dests(ctx, dst, p.dst_base, p.dst_base_len);

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dests.addrs.front();
    node.group_addrs = dests.addrs;
    node.group_vids = dests.vids;
    node.src_vids = *svids;
    node.entry = "hazeBasisConvert";
    node.thunk = [svids = std::move(*svids), src_base, dst_base,
                  dv = dests.vids](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        auto src_mrp = build_lowered_mrp(lower, svids, src_base);
        if (!src_mrp)
            return std::unexpected(src_mrp.error());
        const fhetch::ModuliBase target_base(dst_base.begin(), dst_base.end());
        const fhetch::MRP result = fhetch::fast_base_convert(*src_mrp, target_base);
        for (size_t i = 0; i < dv.size(); ++i)
            lower.bind(dv[i], result[dst_base[i]]);
        return {};
    };
    ctx.tape.append(std::move(node));
    return record_mrp_out_group(ctx, dests.addrs, p.dst_base, p.dst_base_len);
}

std::expected<void, HazeInternalError> mod_down(Context &ctx, void *const *dst,
                                                const void *const *src,
                                                const hazeModDownParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);

    auto svids = record_mrp_sources(ctx, src, p.src_base, p.src_base_len, cfg->ring_dim);
    if (!svids)
        return std::unexpected(svids.error());

    // rescale_fbc returns base == src_base \ rescale_base in src_base's
    // original order; precompute it so the dst residues can be bound at
    // record time. The thunk cross-checks against the gadget's actual
    // result base so HAZE-side and backend-side can never silently
    // disagree on the dst layout.
    const std::vector<uint64_t> src_base(p.src_base, p.src_base + p.src_base_len);
    const std::vector<uint64_t> rescale_base(p.rescale_base, p.rescale_base + p.rescale_base_len);
    std::vector<uint64_t> dst_base;
    dst_base.reserve(p.src_base_len - p.rescale_base_len);
    {
        const std::unordered_set<uint64_t> drop(rescale_base.begin(), rescale_base.end());
        for (uint64_t q : src_base)
            if (!drop.contains(q))
                dst_base.push_back(q);
    }
    MrpDests dests = record_mrp_dests(ctx, dst, dst_base.data(), dst_base.size());

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dests.addrs.front();
    node.group_addrs = dests.addrs;
    node.group_vids = dests.vids;
    node.src_vids = *svids;
    node.entry = "hazeModDown";
    node.thunk = [svids = std::move(*svids), src_base, rescale_base, dst_base,
                  dv = dests.vids](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        auto src_mrp = build_lowered_mrp(lower, svids, src_base);
        if (!src_mrp)
            return std::unexpected(src_mrp.error());
        const fhetch::ModuliBase rb(rescale_base.begin(), rescale_base.end());
        const fhetch::MRP result = fhetch::rescale_fbc(*src_mrp, rb);
        if (result.base() != dst_base) {
            record_internal_error(HazeInternalError::BackendShapeMismatch,
                                  "hazeModDown: rescale_fbc returned an unexpected base");
            return std::unexpected(HazeInternalError::BackendShapeMismatch);
        }
        for (size_t i = 0; i < dv.size(); ++i)
            lower.bind(dv[i], result[dst_base[i]]);
        return {};
    };
    ctx.tape.append(std::move(node));
    return record_mrp_out_group(ctx, dests.addrs, dst_base.data(), dst_base.size());
}

std::expected<void, HazeInternalError>
mod_up(Context &ctx, void *const *dst, const void *const *src, const hazeModUpParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }
    const ConfigSnapshot *cfg = record_prelude(ctx);
    if (cfg == nullptr)
        return std::unexpected(HazeInternalError::NotConfigured);

    auto svids = record_mrp_sources(ctx, src, p.src_base, p.src_base_len, cfg->ring_dim);
    if (!svids)
        return std::unexpected(svids.error());

    std::vector<std::vector<uint64_t>> digit_bases;
    digit_bases.reserve(p.digit_count);
    size_t offset = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        const size_t dlen = p.digit_base_lens[i];
        digit_bases.emplace_back(p.digit_bases + offset, p.digit_bases + offset + dlen);
        offset += dlen;
    }

    // Every digit is base-extended to src_base ∪ p_base, in that
    // concatenation order (dig_decomp builds target_base = x.base() then
    // appends p_base). Precompute it so all digits' dst residues can be
    // bound at record time; the thunk cross-checks per digit.
    const std::vector<uint64_t> src_base(p.src_base, p.src_base + p.src_base_len);
    std::vector<uint64_t> digit_dst_base(src_base);
    digit_dst_base.insert(digit_dst_base.end(), p.p_base, p.p_base + p.p_base_len);
    const size_t stride = digit_dst_base.size();

    std::vector<uint64_t> flat_dst_base;
    flat_dst_base.reserve(stride * p.digit_count);
    for (size_t d2 = 0; d2 < p.digit_count; ++d2)
        flat_dst_base.insert(flat_dst_base.end(), digit_dst_base.begin(), digit_dst_base.end());
    MrpDests dests = record_mrp_dests(ctx, dst, flat_dst_base.data(), flat_dst_base.size());

    Node node{};
    node.kind = Node::Kind::Compute;
    node.addr = dests.addrs.front();
    node.group_addrs = dests.addrs;
    node.group_vids = dests.vids;
    node.src_vids = *svids;
    node.entry = "hazeModUp";
    node.thunk = [svids = std::move(*svids), src_base, digit_bases,
                  p_base = std::vector<uint64_t>(p.p_base, p.p_base + p.p_base_len), digit_dst_base,
                  stride, digit_count = p.digit_count,
                  dv = dests.vids](LowerCtx &lower) -> std::expected<void, HazeInternalError> {
        auto src_mrp = build_lowered_mrp(lower, svids, src_base);
        if (!src_mrp)
            return std::unexpected(src_mrp.error());
        std::vector<fhetch::ModuliBase> db;
        db.reserve(digit_bases.size());
        for (const auto &b : digit_bases)
            db.emplace_back(b.begin(), b.end());
        const fhetch::ModuliBase pb(p_base.begin(), p_base.end());
        const fhetch::MRPArray result = fhetch::dig_decomp(*src_mrp, db, pb);
        if (result.length() != digit_count) {
            record_internal_error(HazeInternalError::BackendShapeMismatch,
                                  "hazeModUp: dig_decomp returned wrong length");
            return std::unexpected(HazeInternalError::BackendShapeMismatch);
        }
        for (size_t d = 0; d < digit_count; ++d) {
            if (result[d].base() != digit_dst_base) {
                record_internal_error(HazeInternalError::BackendShapeMismatch,
                                      "hazeModUp: dig_decomp digit has an unexpected base");
                return std::unexpected(HazeInternalError::BackendShapeMismatch);
            }
            for (size_t i = 0; i < stride; ++i)
                lower.bind(dv[(d * stride) + i], result[d][digit_dst_base[i]]);
        }
        return {};
    };
    ctx.tape.append(std::move(node));

    // Register each digit's dst residues as their own MRP output group,
    // matching the eager engine's per-digit store_mrp registration.
    for (size_t d = 0; d < p.digit_count; ++d) {
        const std::span<const DevAddr> digit_addrs(dests.addrs.data() + (d * stride), stride);
        if (auto registered = record_mrp_out_group(ctx, digit_addrs, digit_dst_base.data(), stride);
            !registered)
            return registered;
    }
    return {};
}

} // namespace haze
