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
// A non-local flush's replay output is pinned inside the program directory,
// rather than the directories nbcc_fhetch_replay would otherwise derive
// relative to its own (inherited) CWD -- see compiler.h's replay_project()
// doc comment. The recording (optimization_stats.json, the record-phase
// timing_summary.json) lands in niobium::replay_output_directory(program_dir)
// and the run (the accumulated timing_summary.json) in
// niobium::replay_run_directory(program_dir).
// hazeReplayConfig::remove_program_directory deletes the whole program
// directory on hazeDeviceReset, so this test also checks that the nested
// replay output goes with it.
//
// Excluded by design: the compiler's own per-process
// timing_summary_<role>_<pid>.json (NB_TIMING_SUMMARY_DIR if set, else the
// CWD, regardless of --out; see compiler.h and haze_types.h). This test
// doesn't assert against that file.

#include "filesystem_helpers.hpp"
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr uint64_t kModulus = 576460752303415297ULL; // standard CKKS test prime (see
                                                     // integration_helpers.hpp)

} // namespace

TEST_CASE("a transport flush nests the replay output directory inside the program directory",
          "[integration]") {
    // Only a real (non-local) replay dispatch spawns nbcc_fhetch_replay at
    // all; the in-process simulator writes no such directory anywhere.
    if (!haze::test::transport_target_active())
        SKIP("replay output scoping only applies to a real replay dispatch; run under make "
             "test-transport");
    // The forwarder unpacks the project server-side (a mkdtemp directory the
    // client never sees), so there is no local stray directory to rule out
    // and no local replay_output_directory() to check either.
    if (haze::test::http_transport_active())
        SKIP("the http forwarder's project unpacks server-side (see integration_helpers.hpp)");

    const fs::path dir = haze::test::make_unique_temp_dir("haze_replay_output_scope_test");
    const fs::path out_dir = niobium::replay_output_directory(dir);
    const fs::path run_dir = niobium::replay_run_directory(dir);
    // Without --out, nbcc_fhetch_replay derives the same names relative to its
    // CWD (compiler.h's replay_output_directory doc comment); run_dir is
    // <out_dir>.run/<name>.
    const fs::path stray = fs::current_path() / out_dir.filename();
    const fs::path stray_run = fs::current_path() / run_dir.parent_path().filename();

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kModulus};
    const hazeFheParams fhe = {.ring_dim = kRingDim, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .program_directory = dir.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kModulus) == HAZE_SUCCESS);

    haze::test::run_minimal_add(kRingDim);

    // Nothing landed next to the test binary's own cwd ...
    CHECK(!fs::exists(stray));
    CHECK(!fs::exists(stray_run));
    // ... the recording and the run are nested inside the program directory
    // instead.
    CHECK(fs::exists(out_dir / "optimization_stats.json"));
    CHECK(fs::exists(out_dir / "timing_summary.json"));
    CHECK(fs::exists(run_dir / "timing_summary.json"));

    hazeReplayBridgeReset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // remove_program_directory deleted the whole program directory, which the
    // replay output nests inside -- so both are gone, with no separate
    // cleanup step of this test's own.
    CHECK(!fs::exists(dir));
    CHECK(!fs::exists(out_dir));
    CHECK(!fs::exists(run_dir));
}
