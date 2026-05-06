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

// MRP polymap glue shared by the basis-convert family and the MRP compute
// templates. The two helpers here are the contract between haze's per-residue
// DevAddr layout and niobium-fhetch's MRP value type:
//
//   - build_mrp_locked: read K residues out of the polymap (or promote them
//     from shadow buffers on first reference) and assemble an MRP carrying
//     the supplied moduli base. Each lookup returns a *copy* of the bound
//     Polynomial, so in-place compute (dst[i] == src1[i] / src2[i]) stays
//     correct without further work.
//
//   - store_mrp_locked: decompose an MRP back into K per-residue DevAddrs,
//     storing each residue via store_compute_result_locked so the standard
//     output-tagging and replay-population paths apply uniformly to MRP
//     results.
//
// Both helpers run under the EpochSession lock; the HAZE_REQUIRES annotation
// keeps clang TSA enforcing that contract at every call site.

std::expected<niobium::fhetch::MRP, HazeInternalError>
build_mrp_locked(const void *const *polys, const uint64_t *base, std::size_t len)
    HAZE_REQUIRES(epoch().mutex());

void store_mrp_locked(void *const *dst_polys, const niobium::fhetch::MRP &mrp, const uint64_t *base,
                      std::size_t len) HAZE_REQUIRES(epoch().mutex());

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
