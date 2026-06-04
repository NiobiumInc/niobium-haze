// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Internal-only test helper that reaches into niobium::compiler() / the fhetch
// result API to cross-check a recorded MRP against per-residue ground truth.
// Split out of integration_helpers.hpp so the latter stays a pure-C-ABI header
// usable by haze_e2e_tests (which links the shipped libhaze.so, where the
// niobium:: symbols are hidden). Only haze_internal_tests (which links the haze
// object files) may include this.

#pragma once

#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <string>
#include <vector>

namespace haze::test {

// Cross-check an MRP output by content (not auto-generated name) so op-order
// shifts don't break the test. SRP is the ground truth; this asserts the
// MRP read path returns the same per-residue values.
inline void check_mrp_against_per_residue(const std::vector<uint64_t> &base,
                                          const std::vector<std::vector<uint64_t>> &expected) {
    REQUIRE(expected.size() == base.size());
    namespace fs = std::filesystem;
    const auto probes_dir = niobium::compiler().get_program_directory() / "serialized_probes";
    INFO("scanning " << probes_dir);
    REQUIRE(fs::exists(probes_dir));

    auto residue_matches = [&](const niobium::fhetch::MRP &mrp) -> bool {
        if (mrp.num_residues() != base.size())
            return false;
        const auto &got_base = mrp.base();
        for (std::size_t i = 0; i < base.size(); ++i) {
            if (std::ranges::find(got_base, base[i]) == got_base.end())
                return false;
            if (mrp[base[i]].int_data() != expected[i])
                return false;
        }
        return true;
    };

    // At least one captured group should match; multiple is fine since
    // disk cleanup at hazeReplayBridgeReset prevents stale-file leaks.
    bool found = false;
    std::string matched_name;
    for (const auto &entry : fs::directory_iterator(probes_dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto stem = entry.path().stem().string();
        if (!stem.starts_with("haze_mrp_out_"))
            continue;
        niobium::fhetch::MRP mrp;
        if (!niobium::fhetch::result(stem, mrp))
            continue;
        if (residue_matches(mrp)) {
            found = true;
            matched_name = stem;
            break;
        }
    }
    INFO("matched MRP group: " << (found ? matched_name : std::string{"<none>"}));
    REQUIRE(found);
}

} // namespace haze::test
