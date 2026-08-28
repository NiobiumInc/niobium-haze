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

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kN = 4096;
constexpr uint64_t kQ = 576460752303415297ULL;

// Bring the bridge up far enough that the compiler has pinned a program directory.
fs::path arm_bridge() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);
    const fs::path dir = niobium::compiler().get_program_directory();
    REQUIRE(!dir.empty());
    return dir;
}

// Stand in for what a recording's post-recording hook writes.
void plant_artifacts(const fs::path &dir) {
    std::error_code ec;
    for (const auto *sub : {"serialized_probes", "ciphertext_templates"}) {
        fs::create_directories(dir / sub, ec);
        std::ofstream f(dir / sub / "marker.txt");
        f << "marker";
    }
    REQUIRE(fs::exists(dir / "serialized_probes" / "marker.txt"));
    REQUIRE(fs::exists(dir / "ciphertext_templates" / "marker.txt"));
}

} // namespace

// hazeWriteProgram() promises a complete project directory for replay elsewhere. A teardown that
// deleted part of it would break that promise, which is why the artifact clear lives at bridge
// init and not at reset.
TEST_CASE("a published project keeps its artifacts across reset", "[replay_bridge]") {
    const fs::path dir = arm_bridge();
    plant_artifacts(dir);

    hazeReplayBridgeReset();

    CHECK(fs::exists(dir / "serialized_probes" / "marker.txt"));
    CHECK(fs::exists(dir / "ciphertext_templates" / "marker.txt"));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The hygiene the clear used to provide at reset still happens, one epoch later: a recording that
// is about to write fresh artifacts starts from a clean directory, so stale names from a previous
// epoch cannot pollute this one's lookups.
TEST_CASE("a new recording starts from a clean artifact directory", "[replay_bridge]") {
    const fs::path first = arm_bridge();
    plant_artifacts(first);

    // Re-arming is what a second context does; the clear belongs to that transition.
    const fs::path second = arm_bridge();
    REQUIRE(second == first); // same process, same pinned program directory

    CHECK(!fs::exists(second / "serialized_probes" / "marker.txt"));
    CHECK(!fs::exists(second / "ciphertext_templates" / "marker.txt"));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::error_code ec;
    fs::remove_all(second, ec);
}
