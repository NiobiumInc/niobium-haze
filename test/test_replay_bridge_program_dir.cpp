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
// Regression: the replay bridge must honor a caller-pinned program directory.
//
// hazeReplayBridgeInitCryptoContext() plants the program name "haze" (so the
// project doesn't land under the "niobium_trace" default). set_program_info()
// also resets the project directory to cwd/<name>. A library integrator (e.g.
// FIDESlib's HazeEngine) pins a custom project directory via
// hazeSetProgramDirectory() BEFORE calling the bridge; if the bridge's rename
// resets it, cryptocontext.dat is written under cwd/haze/ while the .fhetch
// trace lands in the pinned dir. nbcc_fhetch_replay --project=<pinned> then
// fails with "Cannot load crypto context — skipping probe serialization" and
// returns no probes, so the transport readback fails.
//
// The existing suites never set a custom program directory (they use the
// cwd/<name> default), so cryptocontext.dat and the trace always coincided and
// the divergence stayed hidden. This test pins a directory outside cwd and
// asserts the bridge preserves it.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <niobium/compiler.h>   // IWYU pragma: keep
#include <system_error>

namespace fs = std::filesystem;

namespace {
constexpr uint64_t kN = 4096;
constexpr uint64_t kQ = 576460752303415297ULL; // standard CKKS test prime
} // namespace

TEST_CASE("replay bridge honors a caller-pinned program directory", "[replay_bridge]") {
    // A project directory that is NOT the cwd/<program_name> default — exactly
    // what a library integrator pins via hazeSetProgramDirectory().
    const fs::path pinned = fs::temp_directory_path() / "haze_replay_bridge_progdir_test";
    std::error_code ec;
    fs::remove_all(pinned, ec);
    REQUIRE(fs::create_directories(pinned, ec));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);
    // Pin the project directory BEFORE the bridge init, mirroring the HazeEngine
    // bring-up order (hazeSetProgramDirectory precedes the first compute call).
    // This stores the directory in haze::config(); the compiler only learns it at
    // bring-up, so niobium::compiler().get_program_directory() is still the default
    // here — the bridge init is what must propagate it.
    REQUIRE(hazeSetProgramDirectory(pinned.c_str()) == HAZE_SUCCESS);

    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);

    // The bridge's set_program_info("haze") must NOT relocate the project away
    // from the pinned directory. Before the fix this returned cwd/haze, orphaning
    // cryptocontext.dat from the .fhetch trace.
    CHECK(niobium::compiler().get_program_directory() == pinned);

    // And cryptocontext.dat must land in the pinned directory (where the trace
    // and templates are written), so nbcc_fhetch_replay --project=<pinned> finds it.
    CHECK(fs::exists(pinned / "cryptocontext.dat"));

    hazeReplayBridgeReset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(pinned, ec);
}
