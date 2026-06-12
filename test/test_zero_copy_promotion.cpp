// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Pointer-equality test for shadow → Polynomial zero-copy input
// promotion; mirrors lookup_or_create_locked without compute/replay so
// the asserts depend only on move semantics in extract + from_data.
#include "allocator_test_access.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/context.hpp"
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <utility>
#include <vector>

TEST_CASE("zero-copy input promotion: shadow buffer flows into Polynomial without realloc",
          "[unit]") {
    constexpr uint64_t ring_dim = 4096;
    constexpr size_t bytes = ring_dim * sizeof(uint64_t);

    haze::test::recreate_ctx(ring_dim, {});

    void *dev = nullptr;
    REQUIRE(hazeMalloc(haze::test::ctx(), &dev, bytes) == HAZE_SUCCESS);

    std::vector<uint64_t> host(ring_dim);
    for (size_t i = 0; i < ring_dim; ++i) {
        host[i] = (i * 17U) + 3U;
    }
    REQUIRE(hazeMemcpy(haze::test::ctx(), dev, host.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);

    const haze::DevAddr addr = haze::to_dev_addr(dev);
    // Snapshot the shadow address under the lock; only used below as a
    // value-comparison landmark, never dereferenced.
    const uint64_t *shadow_before = nullptr;
    const bool had_shadow = haze::test::AllocatorTestAccess::with_shadow_data(
        haze::test::ctx()->allocator, addr,
        [&](const uint64_t *data, size_t /*size*/) { shadow_before = data; });
    REQUIRE(had_shadow);
    REQUIRE(shadow_before != nullptr);

    // Mirror lookup_or_create_locked's promotion sequence directly so
    // a regression in either step shows up at the matching REQUIRE.
    auto components = haze::test::ctx()->allocator.extract_polynomial_components(addr, ring_dim);
    REQUIRE(components.has_value());
    REQUIRE(components->data() == shadow_before); // step 2: extract is a move

    auto poly = niobium::fhetch::Polynomial::from_data(std::move(*components), ring_dim,
                                                       niobium::fhetch::Format::Evaluation);

    // step 3: from_data(rvalue) — same buffer end-to-end. Relies on
    // vector move-assignment being a pointer-swap; true on libc++ and
    // libstdc++ today, not a standards guarantee.
    REQUIRE(poly.int_data().data() == shadow_before);
    REQUIRE(poly.int_data().size() == ring_dim);
    for (size_t i = 0; i < ring_dim; ++i) {
        REQUIRE(poly.int_data()[i] == (i * 17U) + 3U);
    }

    REQUIRE(hazeFree(haze::test::ctx(), dev) == HAZE_SUCCESS);
}
