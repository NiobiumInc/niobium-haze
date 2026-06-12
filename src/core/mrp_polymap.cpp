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
#include "core/epoch.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::expected<fhetch::MRP, HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, std::size_t len) {
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    std::vector<DevAddr> addrs;
    pairs.reserve(len);
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr a = to_dev_addr(polys[i]);
        auto poly = epoch().lookup_or_create_locked(a);
        if (!poly) {
            return std::unexpected(poly.error());
        }
        pairs.emplace_back(std::move(*poly), base[i]);
        addrs.push_back(a);
    }
    auto mrp = fhetch::MRP::from_pairs(pairs);
    // Additive MRP-input tag on top of the per-addr SRP inputs from
    // lookup_or_create_locked; lets the bridge synthesize a multi-tower
    // input CT and readers pull via fhetch::result(name, MRP&). The name is
    // stable per leading addr, so the EpochState dedup collapses redundant
    // tags when the same source MRP feeds multiple ops.
    if (len > 1) {
        auto group_name = epoch().mrp_group_name_locked(/*output=*/false, addrs.front());
        epoch().tag_mrp_input_if_new_locked(group_name, mrp);
    }
    return mrp;
}

std::expected<void, HazeInternalError> store_mrp_locked(void *const *dst_polys,
                                                        const fhetch::MRP &mrp,
                                                        const uint64_t *base, std::size_t len) {
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr a = to_dev_addr(dst_polys[i]);
        epoch().store_compute_result_locked(a, mrp[base[i]], base[i]);
        addrs.push_back(a);
    }
    if (len > 1) {
        auto group_name = epoch().mrp_group_name_locked(/*output=*/true, addrs.front());
        return epoch().register_mrp_output_group_locked(addrs, std::span(base, len),
                                                        std::move(group_name));
    }
    return {};
}

std::expected<void, HazeInternalError> copy_h2d_mrp(void *const *dst, const void *const *src,
                                                    std::size_t count, std::size_t len) noexcept {
    // Write every residue's shadow first, then tag them under one session:
    // tag promotes the just-written bytes to a fhetch input per residue.
    for (std::size_t i = 0; i < len; ++i)
        if (auto h2d = allocator().copy_h2d(to_dev_addr(dst[i]), src[i], count); !h2d)
            return h2d;
    EpochSession session;
    for (std::size_t i = 0; i < len; ++i)
        if (auto tag = epoch().tag_h2d_input_locked(to_dev_addr(dst[i])); !tag)
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
    EpochSession session;
    std::vector<DevAddr> addrs;
    addrs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        DevAddr d = to_dev_addr(dst[i]);
        if (auto copied = epoch().copy_result_locked(d, to_dev_addr(src[i]), base[i]); !copied)
            return copied;
        addrs.push_back(d);
    }
    if (len > 1) {
        auto group_name = epoch().mrp_group_name_locked(/*output=*/true, addrs.front());
        return epoch().register_mrp_output_group_locked(addrs, std::span(base, len),
                                                        std::move(group_name));
    }
    return {};
}

} // namespace haze
