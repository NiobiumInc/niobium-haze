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

#include "haze_errors.hpp"

#include <cstddef>
#include <expected>
#include <haze/haze_types.h>

namespace haze {

// Internal C++ entry points for the CRT basis-conversion composites.
// extern "C" shims in haze_basis_convert_api.cpp validate pointer
// arguments, dispatch here, and map HazeInternalError to hazeError_t.
// Each function does its own pre-flight validation on the params struct
// and opens an EpochSession internally.

std::expected<void, HazeInternalError> basis_convert(void *const *dst, const void *const *src,
                                                     const hazeBasisConvertParams &params) noexcept;

std::expected<void, HazeInternalError> mod_down(void *const *dst, const void *const *src,
                                                const hazeModDownParams &params) noexcept;

std::expected<void, HazeInternalError> mod_up(void *const *dst, const void *const *src,
                                              const hazeModUpParams &params) noexcept;

} // namespace haze
