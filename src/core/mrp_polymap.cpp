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
#include "core/epoch.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// Derive an MRP group name from dst[0] (16-hex-encoded): same op → same
// leading addr → same name, so the EpochState dedup collapses redundant
// tags. Trailing addrs aren't part of the name; haze's allocator doesn't
// reuse dst[0] within a recording, so distinct ops can't collide.
std::string mrp_signature_name(std::string_view prefix, DevAddr leading_addr) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(prefix.size() + 1 + 16);
    out.append(prefix);
    out.push_back('_');
    const uint64_t v = to_uintptr(leading_addr);
    for (int j = 15; j >= 0; --j)
        out.push_back(hex[(v >> (j * 4)) & 0xFU]);
    return out;
}

} // namespace

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
    // input CT and readers pull via fhetch::result(name, MRP&). Address-
    // keyed name dedups when the same source MRP feeds multiple ops.
    if (len > 1) {
        auto group_name = mrp_signature_name("haze_mrp_in", addrs.front());
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
        epoch().store_compute_result_locked(a, mrp[base[i]]);
        addrs.push_back(a);
    }
    if (len > 1) {
        auto group_name = mrp_signature_name("haze_mrp_out", addrs.front());
        return epoch().register_mrp_output_group_locked(addrs, std::span(base, len),
                                                        std::move(group_name));
    }
    return {};
}

} // namespace haze
