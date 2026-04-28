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

#include <niobium/fhetch_api.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace haze {

// Read the integer-component values out of a fhetch::Polynomial.
//
// fhetch::Polynomial's data is opaque (PolynomialImpl is forward-declared
// in the public header). The only public path to inspect a replayed
// polynomial's values today is to round-trip through
// fhetch::save_polynomial_json and parse the file. This wrapper hides
// the round-trip + parse behind a single call so the materialization
// engine doesn't need to carry that detail.
//
// TODO(niobium-fhetch): replace with Polynomial::int_data() when
// upstream adds it; the round-trip would then become a one-line read of
// the in-memory components vector. Tracked separately because the
// upstream API change is multi-repo.
//
// `tag` is used in the temp filename for diagnostic clarity if multiple
// extractions race. Returns true on success and populates `out` with
// the values; false on any I/O or parse failure.
bool extract_polynomial_values(const niobium::fhetch::Polynomial &p, std::string_view tag,
                               std::vector<uint64_t> &out);

} // namespace haze
