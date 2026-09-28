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

#include <cstdint>
#include <niobium/fhetch_api.h>
#include <span>
#include <vector>

namespace haze {

// Exact CRT base conversion. fhetch's fast base conversion leaves a small
// multiple of the source modulus product in every output; the gadgets here
// reproduce the source integer itself.

// Exact single-prime lift. Each coefficient of v is a residue in [0, q), read
// as the integer of the same value; the result holds that integer mod p for
// each p in dst_base, index-aligned. Every p in dst_base differs from q.
//
// The recorded chain is, with h = (q-1)/2, sub_scalar(h) at q, then per p
// emit_centered_switch (add_scalar(h) at q, a cross-prime rebase to p,
// add_scalar(-h mod p) at p) and add_scalar(h mod p). The two shift pairs
// cancel, so the lift is exact for any q. They are recorded for the hardware
// replay driver: its switchmod rule recognizes this shape and replaces it with
// its own SwitchModulus, while a bare cross-prime rebase has no immediates
// valid in both data formats and replays only on a driver that re-domains
// cross-modulus multiplies. A driver may rewrite the chain with other rules
// too; the recorded trace does not depend on the driver.
// hazeReplayConfig::reduced_noise does not affect the lift.
std::vector<niobium::fhetch::Polynomial> raw_lift(const niobium::fhetch::Polynomial &v, uint64_t q,
                                                  std::span<const uint64_t> dst_base) noexcept;

// Exact base conversion, the exact counterpart of fhetch::fast_base_convert. x
// holds the residues on x.base() = (b_0, ..., b_{k-1}) of the integer X in
// [0, b_0 * ... * b_{k-1}); the result holds X mod r for each r in
// target_base, in target_base order, and a copy of x at every prime the two
// bases share. Garner's mixed radix writes X = sum_t y_t * M_t with
// M_t = b_0 * ... * b_{t-1} and y_t in [0, b_t): each y_t is solved at b_t from
// the partial sum of the digits before it, then raw_lift carries it to every
// prime whose sum is still open.
niobium::fhetch::MRP exact_base_convert(const niobium::fhetch::MRP &x,
                                        const niobium::fhetch::ModuliBase &target_base) noexcept;

// Exact CRT rescale, the exact counterpart of fhetch::rescale_fbc. x holds the
// residues on x.base() of the integer X in [0, prod x.base()); rescale_base is a
// proper subset of x.base() with product P. The result holds
// floor(X / P) mod q for each q of x.base() \ rescale_base, in x.base() order.
// It peels one rescale prime r at a time, X <- (X - raw_lift(X mod r)) / r at
// every prime still held, which is floor(X / r) exactly; the peels compose to
// floor(X / P).
niobium::fhetch::MRP exact_rescale(const niobium::fhetch::MRP &x,
                                   const niobium::fhetch::ModuliBase &rescale_base) noexcept;

} // namespace haze
