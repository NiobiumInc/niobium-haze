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

#include "core/mrp_polymap.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/config.hpp"
#include "core/graph.hpp"
#include "core/lower.hpp"
#include "core/record.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::expected<fhetch::MRP, HazeInternalError> build_lowered_mrp(const LowerCtx &ctx,
                                                                const std::vector<ValueId> &vids,
                                                                const std::vector<uint64_t> &base) {
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(vids.size());
    for (std::size_t i = 0; i < vids.size(); ++i) {
        const auto poly = ctx.poly(vids[i]);
        if (!poly)
            return std::unexpected(poly.error());
        pairs.emplace_back(**poly, base[i]);
    }
    return fhetch::MRP::from_pairs(pairs);
}

std::expected<std::vector<ValueId>, HazeInternalError>
record_mrp_sources(const void *const *polys, const uint64_t *base, std::size_t len,
                   uint64_t ring_dim) noexcept {
    std::vector<ValueId> vids;
    std::vector<DevAddr> addrs;
    vids.reserve(len);
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const DevAddr a = to_dev_addr(polys[i]);
        const auto vid = resolve_operand(a, ring_dim);
        if (!vid)
            return std::unexpected(vid.error());
        vids.push_back(*vid);
        addrs.push_back(a);
    }
    // Additive MRP-input tag on top of the per-addr SRP inputs from the
    // residues' promotion; lets the bridge synthesize a multi-tower
    // input CT and readers pull via fhetch::result(name, MRP&).
    // Address-keyed name dedups (via derive()) when the same source MRP
    // feeds multiple ops.
    if (len > 1) {
        Node node{};
        node.kind = Node::Kind::MrpInputTag;
        node.addr = addrs.front(); // leading addr names the group at derive time
        node.group_addrs = addrs;
        node.group_moduli.assign(base, base + len);
        node.group_vids = vids;
        node.entry = "haze mrp input tag";
        // The counter-based haze_mrp_in_N name comes back through
        // ctx.node_name(), assigned by derive() from the leading addr
        // (mirror of the eager engine's mrp_group_name_locked).
        node.thunk = [vids, base_vec = std::vector<uint64_t>(base, base + len)](
                         LowerCtx &ctx) -> std::expected<void, HazeInternalError> {
            if (!ctx.emit_mrp_input())
                return {}; // a same-named tag already reached fhetch
            auto mrp = build_lowered_mrp(ctx, vids, base_vec);
            if (!mrp)
                return std::unexpected(mrp.error());
            fhetch::tag_input(ctx.node_name(), *mrp);
            return {};
        };
        graph().append(std::move(node));
    }
    return vids;
}

MrpDests record_mrp_dests(void *const *dst_polys, const uint64_t *base, std::size_t len) noexcept {
    MrpDests dests;
    dests.addrs.reserve(len);
    dests.vids.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const DevAddr a = to_dev_addr(dst_polys[i]);
        dests.addrs.push_back(a);
        dests.vids.push_back(bind_result(a, base[i])); // residue carries its prime
    }
    return dests;
}

std::expected<void, HazeInternalError> record_mrp_out_group(std::span<const DevAddr> addrs,
                                                            const uint64_t *base,
                                                            std::size_t len) noexcept {
    if (len <= 1)
        return {};
    // One device address per modulus; a mismatch is a programming bug in
    // the fan-out helper, surfaced rather than dropped silently.
    if (addrs.size() != len) {
        std::ostringstream body;
        body << "record_mrp_out_group: addrs.size()=" << addrs.size() << " != base_len=" << len;
        record_internal_error(HazeInternalError::MrpGroupAddrModuliMismatch, body.str().c_str());
        return std::unexpected(HazeInternalError::MrpGroupAddrModuliMismatch);
    }
    Node node{};
    node.kind = Node::Kind::MrpRegister;
    node.addr = addrs.front(); // leading addr names the group at derive time
    node.group_addrs.assign(addrs.begin(), addrs.end());
    node.group_moduli.assign(base, base + len);
    node.entry = "haze mrp group register";
    graph().append(std::move(node));
    return {};
}

std::expected<void, HazeInternalError> copy_h2d_mrp(void *const *dst, const void *const *src,
                                                    std::size_t count, std::size_t len) noexcept {
    // Write every residue's shadow first, then tag them: each tag
    // snapshots the just-written bytes as a fhetch input per residue.
    for (std::size_t i = 0; i < len; ++i)
        if (auto h2d = allocator().copy_h2d(to_dev_addr(dst[i]), src[i], count); !h2d)
            return h2d;
    for (std::size_t i = 0; i < len; ++i)
        if (auto tag = tag_h2d_input(to_dev_addr(dst[i])); !tag)
            return tag;
    return {};
}

std::expected<void, HazeInternalError> copy_to_host_mrp(void *const *dst, const void *const *src,
                                                        std::size_t count,
                                                        std::size_t len) noexcept {
    // Per-residue pure shadow read; the caller must have tagged the group and
    // flushed, or each residue errors as OutputNotFlushed.
    for (std::size_t i = 0; i < len; ++i)
        if (auto d2h = copy_to_host(dst[i], to_dev_addr(src[i]), count); !d2h)
            return d2h;
    return {};
}

std::expected<void, HazeInternalError> copy_device_to_device_mrp(void *const *dst,
                                                                 const void *const *src,
                                                                 const uint64_t *base,
                                                                 std::size_t len) noexcept {
    // Per-residue pass-through copy, then register the dst as an MRP output
    // group under the real base[i] so it reads back as an MRP, matching the
    // arithmetic MRP ops.
    const ConfigSnapshot *cfg = record_prelude();
    if (cfg == nullptr) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "hazeMemcpyMrp D2D before hazeSetRingDimension");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    const uint64_t ring_dim = cfg->ring_dim;
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const DevAddr d = to_dev_addr(dst[i]);
        if (auto copied = record_copy(d, to_dev_addr(src[i]), ring_dim, base[i]); !copied)
            return copied;
        addrs.push_back(d);
    }
    return record_mrp_out_group(addrs, base, len);
}

} // namespace haze
