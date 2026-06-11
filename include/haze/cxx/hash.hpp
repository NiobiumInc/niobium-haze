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
// param_hash<T>: the customization point every kernel memo-key
// parameter must satisfy. Unused by the M2 layer at runtime (kernels do
// not memoize yet) but REQUIRED by the kernel_param concept from day
// one, so signatures will not churn when memoization starts keying on
// these values. mix() folds a value into a 64-bit FNV-1a-style state.
#pragma once

#include "haze/cxx/handles.hpp"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>

namespace haze::cxx::inline v1 {

namespace detail {

constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

constexpr uint64_t mix_word(uint64_t word, uint64_t seed) noexcept {
    return (seed ^ word) * kFnvPrime;
}

} // namespace detail

template <class T> struct param_hash; // primary: undefined => not a memo key

template <class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
struct param_hash<T> {
    static constexpr uint64_t mix(T value, uint64_t seed) noexcept {
        return detail::mix_word(static_cast<uint64_t>(value), seed);
    }
};

template <> struct param_hash<ModSlot> {
    static constexpr uint64_t mix(const ModSlot &slot, uint64_t seed) noexcept {
        return detail::mix_word(static_cast<uint64_t>(slot.index),
                                detail::mix_word(slot.value, seed));
    }
};

template <> struct param_hash<std::string> {
    static uint64_t mix(const std::string &value, uint64_t seed) noexcept {
        for (const char c : value)
            seed = detail::mix_word(static_cast<unsigned char>(c), seed);
        return detail::mix_word(value.size(), seed);
    }
};

// Contiguous uint64_t ranges (std::vector<uint64_t>, spans of primes,
// per-residue scalar arrays) — load-bearing for the MRP scalar ops.
template <class R>
    requires std::ranges::contiguous_range<R> &&
             std::same_as<std::ranges::range_value_t<R>, uint64_t>
struct param_hash<R> {
    static constexpr uint64_t mix(const R &range, uint64_t seed) noexcept {
        for (const uint64_t v : range)
            seed = detail::mix_word(v, seed);
        return detail::mix_word(std::ranges::size(range), seed);
    }
};

} // namespace haze::cxx::inline v1
