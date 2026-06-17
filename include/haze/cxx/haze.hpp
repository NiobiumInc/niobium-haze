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
//
// Umbrella header for the typed kernel-authoring layer.
//
// Header-only, C++20 floor, lowers exclusively to the C ABI in
// <haze/haze.h> — no C++ symbol ever crosses the libhaze.so boundary,
// so libhaze's internal language level and toolchain stay decoupled
// from the consumer's. The inline namespace versions the layer's
// inline/template code against future ABI-affecting revisions
// (libcu++-style).
#pragma once

#include "haze/cxx/error.hpp"   // IWYU pragma: export
#include "haze/cxx/handles.hpp" // IWYU pragma: export
#include "haze/cxx/hash.hpp"    // IWYU pragma: export
#include "haze/cxx/kernel.hpp"  // IWYU pragma: export
#include "haze/cxx/ops.hpp"     // IWYU pragma: export
#include "haze/cxx/roles.hpp"   // IWYU pragma: export
