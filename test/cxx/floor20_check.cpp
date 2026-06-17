// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// C++20 floor check for the public typed layer: this TU is compiled at
// exactly -std=c++20 (see the haze_cxx_floor20 target). If a C++23-ism
// leaks into include/haze/cxx/, the BUILD fails — consumers are not
// required to track libhaze's internal language level.

#include <cstdint>
#include <haze/cxx/haze.hpp>

namespace {

using haze::cxx::In;
using haze::cxx::Mrp;
using haze::cxx::Out;
using haze::cxx::Status;

constexpr auto floor_kernel = haze::cxx::kernel(
    "floor20", [](In<Mrp> /*a*/, Out<Mrp> /*dst*/, uint64_t /*k*/) -> Status { return Status{}; });

} // namespace

// The object file needs one external symbol.
int haze_cxx_floor20_anchor() {
    return floor_kernel.name() != nullptr ? 0 : 1;
}
