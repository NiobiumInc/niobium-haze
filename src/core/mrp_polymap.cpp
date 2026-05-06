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
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

std::expected<fhetch::MRP, HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, std::size_t len) {
    std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
    pairs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        auto poly = epoch().lookup_or_create_locked(to_dev_addr(polys[i]));
        if (!poly) {
            return std::unexpected(poly.error());
        }
        pairs.emplace_back(std::move(*poly), base[i]);
    }
    return fhetch::MRP::from_pairs(pairs);
}

void store_mrp_locked(void *const *dst_polys, const fhetch::MRP &mrp, const uint64_t *base,
                      std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        epoch().store_compute_result_locked(to_dev_addr(dst_polys[i]), mrp[base[i]]);
    }
}

} // namespace haze
