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
#include "common/thread_safety.hpp"
#include "core/epoch.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

namespace haze {

// MRP polymap glue: build_mrp_locked assembles K per-addr residues into an
// MRP; store_mrp_locked decomposes one back into per-addr stores. Both run
// under the EpochSession lock (HAZE_REQUIRES enforces it via clang TSA).

std::expected<niobium::fhetch::MRP, HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, std::size_t len)
    HAZE_REQUIRES(epoch().mutex());

std::expected<void, HazeInternalError> store_mrp_locked(void *const *dst_polys,
                                                        const niobium::fhetch::MRP &mrp,
                                                        const uint64_t *base, std::size_t len)
    HAZE_REQUIRES(epoch().mutex());

// Build an MRS from per-modulus uint64_t scalars + their primes. Pure-data
// helper: does not touch the polymap, so no lock contract.
inline niobium::fhetch::MRS build_mrs(const uint64_t *scalars, const uint64_t *base,
                                      std::size_t len) {
    std::vector<std::pair<niobium::fhetch::Scalar, uint64_t>> pairs;
    pairs.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        pairs.emplace_back(niobium::fhetch::Scalar::from_int(scalars[i]), base[i]);
    }
    return niobium::fhetch::MRS::from_pairs(pairs);
}

} // namespace haze
