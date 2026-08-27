// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// Test that HAZE_KEEP_REPLAY_ARTIFACTS environment variable controls whether
// serialized_probes/ and ciphertext_templates/ directories are preserved across
// device reset. When the flag is set and non-empty and first character is not
// '0', both directories survive; otherwise they are removed.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <niobium/compiler.h>   // IWYU pragma: keep
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kN = 4096;
constexpr uint64_t kQ = 576460752303415297ULL;

/// @brief Sets an environment variable for the duration of one test case and restores whatever
/// was there before, on every exit path. Catch2 runs every case in one process, so a REQUIRE
/// failure between a bare setenv and a bare unsetenv would leak the flag into every later case;
/// the guard's destructor runs during that unwind.
class ScopedEnv {
  public:
    ScopedEnv(const char *name, const char *value) : name_(name) {
        if (const char *prior = std::getenv(name)) {
            had_prior_ = true;
            prior_ = prior;
        }
        apply(value);
    }

    ~ScopedEnv() { apply(had_prior_ ? prior_.c_str() : nullptr); }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;
    ScopedEnv(ScopedEnv &&) = delete;
    ScopedEnv &operator=(ScopedEnv &&) = delete;

  private:
    void apply(const char *value) const {
        if (value == nullptr) {
            ::unsetenv(name_.c_str());
        } else {
            ::setenv(name_.c_str(), value, 1);
        }
    }

    std::string name_;
    bool had_prior_ = false;
    std::string prior_;
};

// Helper to count files matching a pattern in a directory.
std::size_t count_files_in_dir(const fs::path &dir) {
    if (!fs::exists(dir))
        return 0;
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file())
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("hazeReplayBridgeReset removes artifacts by default", "[replay_bridge]") {
    // The default behaviour: the flag absent for the whole case, restored on every exit path.
    const ScopedEnv guard("HAZE_KEEP_REPLAY_ARTIFACTS", nullptr);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);

    const fs::path program_dir = niobium::compiler().get_program_directory();
    REQUIRE(!program_dir.empty());

    // Create dummy files in the directories that should be cleaned.
    const fs::path probes_dir = program_dir / "serialized_probes";
    const fs::path templates_dir = program_dir / "ciphertext_templates";
    std::error_code ec;
    fs::create_directories(probes_dir, ec);
    fs::create_directories(templates_dir, ec);

    // Write marker files to verify they get deleted.
    const fs::path probe_marker = probes_dir / "marker.txt";
    const fs::path template_marker = templates_dir / "marker.txt";
    {
        std::ofstream f1(probe_marker);
        f1 << "probe marker";
    }
    {
        std::ofstream f2(template_marker);
        f2 << "template marker";
    }

    // Verify files exist before reset.
    REQUIRE(fs::exists(probe_marker));
    REQUIRE(fs::exists(template_marker));

    // Reset should clean both directories.
    hazeReplayBridgeReset();

    // Verify files are removed.
    CHECK(!fs::exists(probe_marker));
    CHECK(!fs::exists(template_marker));
    // Directories themselves may or may not exist after remove_all; just verify
    // the contents are gone.
    CHECK(count_files_in_dir(probes_dir) == 0);
    CHECK(count_files_in_dir(templates_dir) == 0);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeReplayBridgeReset preserves artifacts when flag is set", "[replay_bridge]") {
    const ScopedEnv guard("HAZE_KEEP_REPLAY_ARTIFACTS", "1");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);

    const fs::path program_dir = niobium::compiler().get_program_directory();
    REQUIRE(!program_dir.empty());

    // Create dummy files in the directories.
    const fs::path probes_dir = program_dir / "serialized_probes";
    const fs::path templates_dir = program_dir / "ciphertext_templates";
    std::error_code ec;
    fs::create_directories(probes_dir, ec);
    fs::create_directories(templates_dir, ec);

    // Write marker files.
    const fs::path probe_marker = probes_dir / "marker.txt";
    const fs::path template_marker = templates_dir / "marker.txt";
    {
        std::ofstream f1(probe_marker);
        f1 << "probe marker";
    }
    {
        std::ofstream f2(template_marker);
        f2 << "template marker";
    }

    // Verify files exist before reset.
    REQUIRE(fs::exists(probe_marker));
    REQUIRE(fs::exists(template_marker));

    // Reset with flag set should preserve both directories and their contents.
    hazeReplayBridgeReset();

    // Verify files still exist.
    CHECK(fs::exists(probe_marker));
    CHECK(fs::exists(template_marker));
    CHECK(count_files_in_dir(probes_dir) == 1);
    CHECK(count_files_in_dir(templates_dir) == 1);

    // The flag is still '1' here -- the guard's destructor runs after this scope -- so the
    // reset below preserves the artifacts too. Removing them is this line's job, not the
    // guard's.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(program_dir, ec);
}

TEST_CASE("HAZE_KEEP_REPLAY_ARTIFACTS set but empty removes artifacts", "[replay_bridge]") {
    // Set-but-empty is the third state the reader distinguishes (v[0] != '\0'), and an empty
    // value is what an unset-looking `export HAZE_KEEP_REPLAY_ARTIFACTS=` actually produces.
    const ScopedEnv guard("HAZE_KEEP_REPLAY_ARTIFACTS", "");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);

    const fs::path program_dir = niobium::compiler().get_program_directory();
    REQUIRE(!program_dir.empty());

    const fs::path probes_dir = program_dir / "serialized_probes";
    const fs::path templates_dir = program_dir / "ciphertext_templates";
    std::error_code ec;
    fs::create_directories(probes_dir, ec);
    fs::create_directories(templates_dir, ec);
    const fs::path probe_marker = probes_dir / "marker.txt";
    const fs::path template_marker = templates_dir / "marker.txt";
    { std::ofstream f(probe_marker); f << "probe marker"; }
    { std::ofstream f(template_marker); f << "template marker"; }
    REQUIRE(fs::exists(probe_marker));
    REQUIRE(fs::exists(template_marker));

    hazeReplayBridgeReset();

    CHECK(!fs::exists(probe_marker));
    CHECK(!fs::exists(template_marker));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("HAZE_KEEP_REPLAY_ARTIFACTS with value '0' removes artifacts", "[replay_bridge]") {
    // '0' must read as off, exactly like absent.
    const ScopedEnv guard("HAZE_KEEP_REPLAY_ARTIFACTS", "0");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kN};
    const hazeReplayConfig replay = {.target = "local"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);

    const fs::path program_dir = niobium::compiler().get_program_directory();
    REQUIRE(!program_dir.empty());

    // Create dummy files in the directories.
    const fs::path probes_dir = program_dir / "serialized_probes";
    const fs::path templates_dir = program_dir / "ciphertext_templates";
    std::error_code ec;
    fs::create_directories(probes_dir, ec);
    fs::create_directories(templates_dir, ec);

    // Write marker files.
    const fs::path probe_marker = probes_dir / "marker.txt";
    const fs::path template_marker = templates_dir / "marker.txt";
    {
        std::ofstream f1(probe_marker);
        f1 << "probe marker";
    }
    {
        std::ofstream f2(template_marker);
        f2 << "template marker";
    }

    // Reset should clean both directories (since '0' means false).
    hazeReplayBridgeReset();

    // Verify files are removed.
    CHECK(!fs::exists(probe_marker));
    CHECK(!fs::exists(template_marker));
    CHECK(count_files_in_dir(probes_dir) == 0);
    CHECK(count_files_in_dir(templates_dir) == 0);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
