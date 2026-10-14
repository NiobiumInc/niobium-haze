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
// hazeReplayConfig::opt_level reaching the compiler-side replay (under make
// test-transport / test-transport-hw), read back from nbcc_fhetch_replay's
// own optimization_stats.json rather than re-derived, so the test fails if
// haze's argv wiring (src/core/backend.cpp) regresses. The out-of-range
// rejection case lives in test_config.cpp beside the other "hazeConfigureDevice
// rejects ..." cases.

#include "filesystem_helpers.hpp"
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <iterator>
#include <niobium/compiler.h>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr uint64_t kModulus = 576460752303415297ULL; // standard CKKS test prime (see
                                                     // integration_helpers.hpp)

// Configures a single-prime SRP context at the given opt_level, pinned to a
// dedicated, unique project directory (haze::test::make_unique_temp_dir) so
// this case's project can't collide with another case's default cwd/haze,
// or with a leftover from an earlier run. Returns the project directory.
fs::path configure_with_opt_level(int opt_level, std::string_view case_name) {
    const fs::path dir =
        haze::test::make_unique_temp_dir("haze_opt_level_test_" + std::string(case_name));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kModulus};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .program_directory = dir.c_str(),
                                     .opt_level = opt_level};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    // A non-local flush fails without the CryptoContext the bridge captures.
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kModulus) == HAZE_SUCCESS);
    return dir;
}

// nbcc_fhetch_replay's per-project secondary work (including
// optimization_stats.json) lives under niobium::replay_output_directory(project_dir),
// nested inside the project directory (see compiler.h's replay_project() doc
// comment). Under HTTP transport this path is on the SERVER, not visible
// here at all -- guard with haze::test::http_transport_active() before using
// it.
fs::path stats_path_for(const fs::path &project_dir) {
    return niobium::replay_output_directory(project_dir) / "optimization_stats.json";
}

// Reads the one field this suite checks out of
// Generator::save_optimization_stats_json's fixed, self-serialized format —
// no JSON library dependency needed for one substring.
std::string read_optimization_level(const fs::path &stats_path) {
    std::ifstream in(stats_path);
    REQUIRE(in.is_open());
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = R"("optimization_level": ")";
    const auto pos = contents.find(kKey);
    REQUIRE(pos != std::string::npos);
    const auto start = pos + kKey.size();
    const auto end = contents.find('"', start);
    REQUIRE(end != std::string::npos);
    return contents.substr(start, end - start);
}

void cleanup(const fs::path &project_dir) {
    std::error_code ec;
    fs::remove_all(project_dir, ec); // the replay's own output nests inside it
    hazeReplayBridgeReset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

struct OptLevelCase {
    int level;
    std::string_view case_name;
    std::string_view want;
};

} // namespace

// Regression guard for the vendor Compiler singleton's opt_level staleness
// (see backend.cpp): O3 runs first and leaves the singleton at "O3", then O0
// must still read back "O0" in the SAME process, not inherit the prior
// value. A parameterized loop over two separate TEST_CASEs would not catch
// this -- Catch2 does not guarantee run order across cases, and this bug was
// first found because it only reproduced when O3 happened to run before O0.
TEST_CASE("opt_level reaches the compiler-side replay, independent of prior state",
          "[integration]") {
    // opt_level only reaches a real replay driver on a non-local target: the
    // in-process simulator (make test-unit / test-sim) has no optimization
    // levels and writes no optimization_stats.json.
    if (!haze::test::transport_target_active())
        SKIP("opt_level has no effect on the local target; run under make test-transport");

    // The forwarder unpacks the project into a server-side mkdtemp directory
    // the client never sees (nbcc_fhetch_replay_source_nbcc_fhetch_server_
    // <rand>/), so there is no local optimization_stats.json to read back
    // under HTTP transport. Skip only the file read, once, after the loop --
    // both iterations' flushes still run as real requests either way.
    const bool http = haze::test::http_transport_active();
    for (const OptLevelCase &c : {OptLevelCase{.level = 3, .case_name = "o3", .want = "O3"},
                                  OptLevelCase{.level = 0, .case_name = "o0", .want = "O0"}}) {
        const fs::path dir = configure_with_opt_level(c.level, c.case_name);
        haze::test::run_minimal_add(kRingDim);

        if (!http) {
            const fs::path stats = stats_path_for(dir);
            REQUIRE(fs::exists(stats));
            REQUIRE(read_optimization_level(stats) == c.want);
        }

        cleanup(dir);
    }

    if (http)
        SKIP("replay output stays on the server");
}
