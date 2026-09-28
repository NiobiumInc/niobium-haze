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
// hazeReplayConfig::remove_program_directory: hazeDeviceReset deletes the
// program directory this session actually recorded into when it's set (0,
// the default, keeps it -- test_written_program_survives.cpp already covers
// that side with an implicit default), and hazeWriteProgram refuses to run
// at all while it's set, since its own contract is to publish a complete
// project for replay elsewhere. The directory removed is the one
// CompilerBackend::ensure_initialized() recorded at bring-up (see
// backend.hpp/.cpp), not one re-derived from the live compiler or the
// working directory at reset time -- see the CWD-change case below, which
// is the regression this file's design guards against.

#include "filesystem_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kN = 4096;
constexpr uint64_t kQ = 576460752303415297ULL; // standard CKKS test prime

// Configures + arms the replay bridge at a pinned, pre-existing project
// directory, satisfying remove_program_directory's requirement of an
// explicit program_directory. Bridge init is what brings the compiler
// backend up (CompilerBackend::ensure_initialized), the point at which
// hazeReplayConfig::program_directory gets resolved and recorded.
void configure_at(const fs::path &dir, int remove_program_directory) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // A real modulus, not just a ring dimension: run_minimal_add() below
    // needs one configured to compute against.
    const uint64_t moduli[] = {kQ};
    const hazeFheParams fhe = {.ring_dim = kN, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.program_directory = dir.c_str(),
                                     .remove_program_directory = remove_program_directory};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ) == HAZE_SUCCESS);
}

// One pointwise add, tagged and flushed: drives a real recording through to
// on-disk artifacts (the .fhetch trace, serialized_probes/<name>.ct), so a
// removal test exercises an actual nested tree rather than just the single
// cryptocontext.dat file bridge init writes.
void run_minimal_add() {
    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    constexpr std::size_t kBytes = kN * sizeof(uint64_t);
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> data(kN, 3);
    REQUIRE(hazeMemcpy(a, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(c) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
}

} // namespace

TEST_CASE("hazeDeviceReset removes the program directory when the flag is set, including a "
          "nested tree written by a real flush",
          "[replay_bridge]") {
    const fs::path dir = haze::test::make_unique_temp_dir("haze_progdir_remove");
    configure_at(dir, /*remove_program_directory=*/1);
    REQUIRE(fs::exists(dir / "cryptocontext.dat")); // bridge init wrote into it
    run_minimal_add();
    // serialized_probes/<name>.ct is written under the program directory by
    // the tag+flush above -- a real nested tree, not just the one file
    // bridge init produces.
    REQUIRE(fs::exists(dir / "serialized_probes"));
    REQUIRE_FALSE(fs::directory_iterator(dir / "serialized_probes") == fs::directory_iterator());

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("hazeDeviceReset keeps the program directory when the flag is unset", "[replay_bridge]") {
    const fs::path dir = haze::test::make_unique_temp_dir("haze_progdir_keep");
    configure_at(dir, /*remove_program_directory=*/0);
    REQUIRE(fs::exists(dir / "cryptocontext.dat"));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    CHECK(fs::exists(dir));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("hazeWriteProgram rejects a configuration with the flag set", "[unit]") {
    const fs::path dir = haze::test::make_unique_temp_dir("haze_progdir_writeprogram");
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.program_directory = dir.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);

    REQUIRE(hazeWriteProgram() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("remove_program_directory") != std::string::npos);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // No compute or bridge init happened in this test, so this session never
    // brought the backend up: hazeDeviceReset() removes nothing (M1), and
    // the directory make_unique_temp_dir created survives it.
    CHECK(fs::exists(dir));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("hazeWriteProgram's rejection does not disturb an active recording, which can still "
          "be flushed afterwards",
          "[replay_bridge]") {
    const fs::path dir = haze::test::make_unique_temp_dir("haze_progdir_writeprogram_active");
    configure_at(dir, /*remove_program_directory=*/1);

    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    constexpr std::size_t kBytes = kN * sizeof(uint64_t);
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> data(kN, 5);
    REQUIRE(hazeMemcpy(a, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, data.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    // Starts an active recording (ensure_recording_locked on first compute).
    REQUIRE(hazeAdd(c, a, b, 0, nullptr) == HAZE_SUCCESS);

    // Rejected because the flag is set, not because of the recording's
    // state -- write_program's check runs unconditionally, before it would
    // otherwise touch the recording at all.
    REQUIRE(hazeWriteProgram() == HAZE_ERROR_INVALID_VALUE);

    // The rejected call must not have cleared or corrupted the in-flight
    // recording: tagging and flushing the same recording afterwards still
    // succeeds and produces a real result.
    REQUIRE(hazeTagOutput(c) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    std::vector<uint64_t> result(kN, 0);
    REQUIRE(hazeMemcpy(result.data(), c, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    CHECK(result[0] == 10); // 5 + 5, the value actually recorded

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// B1 regression guard: hazeDeviceReset must delete the directory THIS
// SESSION actually recorded into, not one derived from the compiler's live
// state (or a relative program_directory) resolved against whatever working
// directory happens to be current when reset() runs. Reproduced without the
// fix: bring-up in root_a/ (program_directory "used", relative), chdir to
// root_b/ (which happens to contain an unrelated root_b/used/ directory of
// its own), reset() -- a live re-query resolves the SAME relative name
// against the NEW cwd and deletes root_b/used/ instead, leaving root_a/used/
// (the directory actually used) behind.
TEST_CASE("hazeDeviceReset deletes the directory this session actually used, not one resolved "
          "against a later working directory",
          "[replay_bridge]") {
    const haze::test::RestoreCwd restore_cwd;

    const fs::path root_a = haze::test::make_unique_temp_dir("haze_b1_bringup_root");
    const fs::path root_b = haze::test::make_unique_temp_dir("haze_b1_other_root");
    const fs::path used_absolute = root_a / "used";
    REQUIRE(fs::create_directories(used_absolute));

    fs::current_path(root_a);
    configure_at("used", /*remove_program_directory=*/1);
    REQUIRE(fs::exists(used_absolute / "cryptocontext.dat"));

    // An unrelated directory that happens to share the SAME relative name,
    // under a DIFFERENT root -- exactly what a live re-query against the new
    // cwd would resolve "used" to. Its content must survive.
    const fs::path collision = root_b / "used";
    REQUIRE(fs::create_directories(collision));
    std::ofstream(collision / "precious.txt") << "keep me";
    REQUIRE(fs::exists(collision / "precious.txt"));

    fs::current_path(root_b);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    CHECK_FALSE(fs::exists(used_absolute));        // the directory actually used is gone
    CHECK(fs::exists(collision / "precious.txt")); // the unrelated same-named dir is untouched

    std::error_code ec;
    fs::remove_all(root_a, ec);
    fs::remove_all(root_b, ec);
}

// N1 regression guard: a relative program_directory is resolved to an
// absolute, canonical path at hazeConfigureDevice time (ReplayConfig::create)
// and THAT is what gets pinned into the compiler at bring-up
// (CompilerBackend::ensure_initialized, triggered here by the bridge init
// call), not re-resolved against whatever working directory happens to be
// current when bring-up actually runs. Without this, a chdir strictly
// between configure and bring-up makes the compiler write into (and this
// later delete) a directory under the NEW working directory that was never
// validated at all -- root_b/used below, which this test asserts never even
// gets created.
TEST_CASE("hazeDeviceReset deletes the directory validated at configure time even after a "
          "chdir before bring-up",
          "[replay_bridge]") {
    const haze::test::RestoreCwd restore_cwd;

    const fs::path root_a = haze::test::make_unique_temp_dir("haze_n1_chdir_a");
    const fs::path root_b = haze::test::make_unique_temp_dir("haze_n1_chdir_b");
    const fs::path used_absolute = root_a / "used";
    REQUIRE(fs::create_directories(used_absolute));

    fs::current_path(root_a);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ};
    const hazeFheParams fhe = {.ring_dim = kN, .moduli = moduli, .moduli_count = 1};
    const hazeReplayConfig replay = {.program_directory = "used", .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);

    // The chdir happens strictly between configure (validated and resolved
    // against root_a above) and bring-up (triggered by the bridge init call
    // below) -- exactly the window a raw, unresolved program_directory
    // would get re-derived in.
    fs::current_path(root_b);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ) == HAZE_SUCCESS);

    // The compiler wrote into the directory validated at configure time, not
    // one resolved against the post-chdir cwd; nothing was ever created
    // under root_b.
    REQUIRE(fs::exists(used_absolute / "cryptocontext.dat"));
    REQUIRE_FALSE(fs::exists(root_b / "used"));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    CHECK_FALSE(fs::exists(used_absolute));
    CHECK_FALSE(fs::exists(root_b / "used"));

    std::error_code ec;
    fs::remove_all(root_a, ec);
    fs::remove_all(root_b, ec);
}

// N1 regression guard: a program_directory crossing a symlink must be
// resolved PHYSICALLY (following the symlink), not lexically. "shallow/link/.."
// lexically cancels "link" and ".." textually, landing back on "shallow" --
// the directory holding the symlink -- regardless of what "link" actually
// points to. But "link" -> deep/target makes the PHYSICAL parent of
// "shallow/link/.." equal to "deep" (target's real parent), a completely
// different directory, and that's where the compiler's own (symlink-
// following) file opens actually land.
TEST_CASE("hazeDeviceReset resolves a symlink-and-parent program_directory physically, not "
          "lexically",
          "[replay_bridge]") {
    const fs::path root = haze::test::make_unique_temp_dir("haze_n1_symlink_dotdot");
    const fs::path deep_parent = root / "deep";          // the physical target's parent
    const fs::path deep_target = deep_parent / "target"; // the real directory: deep/target
    const fs::path shallow = root / "shallow";           // holds the symlink; the LEXICAL parent
    REQUIRE(fs::create_directories(deep_target));
    REQUIRE(fs::create_directories(shallow));
    std::error_code link_ec;
    fs::create_directory_symlink(deep_target, shallow / "link", link_ec);
    REQUIRE_FALSE(link_ec);

    // A sentinel at the LEXICAL interpretation ("shallow") that nothing this
    // session does should ever touch.
    std::ofstream(shallow / "sentinel.txt") << "keep me";
    REQUIRE(fs::exists(shallow / "sentinel.txt"));

    configure_at(shallow / "link" / "..", /*remove_program_directory=*/1);

    // The compiler's own (symlink-following) writes land in the PHYSICAL
    // parent of the symlink's target -- deep/ -- not the lexical parent.
    REQUIRE(fs::exists(deep_parent / "cryptocontext.dat"));
    REQUIRE_FALSE(fs::exists(shallow / "cryptocontext.dat"));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    CHECK_FALSE(fs::exists(deep_parent));        // the directory actually used is gone
    CHECK(fs::exists(shallow / "sentinel.txt")); // the lexical-only interpretation is untouched
    CHECK(fs::exists(shallow));

    std::error_code ec;
    fs::remove_all(root, ec);
}
