// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Verifies the `RingDimChoice` factories on `ops::make_ctx`.

#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>

TEST_CASE("ops::make_ctx with RingDimChoice::OpenFHEDerives() lets OpenFHE pick N",
          "[integration][e2e]") {
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = lbcrypto::FIXEDAUTO,
                              .mult_depth = 1,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .ring_dim = ops::RingDimChoice::OpenFHEDerives()});

    const std::uint32_t derived = ctx.cc->GetRingDimension();
    REQUIRE(ctx.ring_dim == derived);
    // OpenFHE's specific N depends on its security table; assert
    // power-of-two rather than pinning a value.
    REQUIRE(derived > 0);
    REQUIRE((derived & (derived - 1)) == 0);
    REQUIRE(ctx.poly_bytes == static_cast<std::size_t>(ctx.ring_dim) * sizeof(std::uint64_t));
}

TEST_CASE("ops::make_ctx with RingDimChoice::Pinned(N) forces N", "[integration][e2e]") {
    namespace ops = haze::test::ops;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    constexpr std::uint32_t kPinned = 2048;
    auto ctx = ops::make_ctx({.mode = lbcrypto::FIXEDAUTO,
                              .mult_depth = 1,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .ring_dim = ops::RingDimChoice::Pinned(kPinned)});

    REQUIRE(ctx.cc->GetRingDimension() == kPinned);
    REQUIRE(ctx.ring_dim == kPinned);
    REQUIRE(ctx.poly_bytes == static_cast<std::size_t>(kPinned) * sizeof(std::uint64_t));
}
