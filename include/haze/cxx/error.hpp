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
// Error surface for the header-only typed layer. Header-only and C++20:
// nothing here crosses the libhaze.so boundary — the baseline API shape
// is hazeError_t-backed everywhere; std::expected sugar is additive only
// and feature-gated, never a second API.
#pragma once

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <optional>
#include <utility>
#ifdef __cpp_lib_expected
#include <expected>
#endif

namespace haze::cxx::inline v1 {

// A hazeError_t with [[nodiscard]] teeth.
class [[nodiscard]] Status {
  public:
    constexpr Status() noexcept = default;
    constexpr explicit Status(hazeError_t code) noexcept : code_(code) {}

    constexpr bool ok() const noexcept { return code_ == HAZE_SUCCESS; }
    constexpr explicit operator bool() const noexcept { return ok(); }
    constexpr hazeError_t code() const noexcept { return code_; }

  private:
    hazeError_t code_ = HAZE_SUCCESS;
};

// Status + payload for fallible constructors (Srp/Mrp::allocate etc.).
template <class T> class [[nodiscard]] Result {
  public:
    /*implicit*/ Result(T value) noexcept : value_(std::move(value)) {}
    /*implicit*/ Result(Status status) noexcept : status_(status) {}

    bool ok() const noexcept { return status_.ok() && value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    Status status() const noexcept { return status_; }
    hazeError_t code() const noexcept { return status_.code(); }

    // Contract: check ok() first; access on an error Result is UB by
    // design (mirrors std::expected::operator*).
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    T &value() & noexcept { return *value_; }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const T &value() const & noexcept { return *value_; }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    T &&value() && noexcept { return *std::move(value_); }

  private:
    Status status_;
    std::optional<T> value_;
};

#ifdef __cpp_lib_expected
// Additive sugar for C++23 consumers; converts, never replaces.
template <class T> std::expected<T, hazeError_t> to_expected(Result<T> result) {
    if (!result.ok())
        return std::unexpected(result.code());
    return std::move(result).value();
}
#endif

} // namespace haze::cxx::inline v1
