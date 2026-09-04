// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Pointer-equality test for shadow → Polynomial zero-copy move; mirrors the
// extract + from_data sequence lookup_or_create_locked (and the eager H2D tag)
// run internally, in isolation from compute/replay/spill. The shadow is
// written directly via update_shadow rather than hazeMemcpy(H2D): with
// recording active, H2D eagerly tags and moves the shadow into the spill
// store, leaving nothing here to promote.
#include "allocator_test_access.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"

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

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = ring_dim};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);

    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, bytes) == HAZE_SUCCESS);
    const haze::DevAddr addr = haze::to_dev_addr(dev);

    std::vector<uint64_t> host(ring_dim);
    for (size_t i = 0; i < ring_dim; ++i) {
        host[i] = (i * 17U) + 3U;
    }
    auto written = haze::allocator().update_shadow(addr, std::move(host));
    REQUIRE(written.has_value());

    // Snapshot the shadow address under the lock; only used below as a
    // value-comparison landmark, never dereferenced.
    const uint64_t *shadow_before = nullptr;
    const bool had_shadow = haze::test::AllocatorTestAccess::with_shadow_data(
        haze::allocator(), addr,
        [&](const uint64_t *data, size_t /*size*/) { shadow_before = data; });
    REQUIRE(had_shadow);
    REQUIRE(shadow_before != nullptr);

    // Mirror lookup_or_create_locked's promotion sequence directly so
    // a regression in either step shows up at the matching REQUIRE.
    auto components = haze::allocator().extract_polynomial_components(addr, ring_dim);
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

    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
}
