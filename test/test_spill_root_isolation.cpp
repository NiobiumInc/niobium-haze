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
// The input spill root's placement and its lifetime across processes. The root
// sits beside the directory a relative program_directory names. A sibling process
// whose program directory shares this one's parent must leave this process's
// pending inputs on disk when it resets, and a run that dies without a reset must
// have its spill files removed by the next reset under the same program directory.

#include "core/input_spill.hpp"
#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <set>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;
// The hidden sibling cases read their program directory from this variable.
constexpr const char *kSiblingDirVariable = "HAZE_SIBLING_PROGRAM_DIR";
constexpr const char *kResettingSiblingFilter = "[sibling]";
constexpr const char *kCrashingSiblingFilter = "[crashing_sibling]";

void configure(const fs::path &dir, const std::vector<uint64_t> &base) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const std::string dir_string = dir.string();
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = base.data(), .moduli_count = base.size()};
    const hazeReplayConfig replay = {.target = haze::test::target_from_env(),
                                     .program_directory = dir_string.c_str(),
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, base[0]) == HAZE_SUCCESS);
}

std::vector<std::vector<uint64_t>> residues_for(const std::vector<uint64_t> &base, uint64_t seed) {
    std::vector<std::vector<uint64_t>> r(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        r[i] = haze::test::make_residue(base[i], seed + i, kRingDim);
    return r;
}

// x + x recorded over `base` and tagged, not yet flushed.
struct PendingAdd {
    std::vector<std::vector<uint64_t>> x;
    std::vector<void *> src;
    std::vector<void *> dst;
};

PendingAdd record_add(const std::vector<uint64_t> &base, uint64_t seed) {
    PendingAdd add{.x = residues_for(base, seed), .src = {}, .dst = {}};
    add.src = haze::test::allocate_and_h2d_residues(add.x, base);
    add.dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    const auto src_const = haze::test::to_const(add.src);
    REQUIRE(hazeAddMrp(add.dst.data(), src_const.data(), src_const.data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : add.dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    return add;
}

void flush_and_check(const PendingAdd &add, const std::vector<uint64_t> &base) {
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> out(kRingDim);
        REQUIRE(hazeMemcpy(out.data(), add.dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        std::vector<uint64_t> expected(kRingDim);
        for (std::size_t j = 0; j < kRingDim; ++j)
            expected[j] = haze::test::add_mod(add.x[i][j], add.x[i][j], base[i]);
        REQUIRE(out == expected);
    }
    haze::test::free_all_residues(add.src);
    haze::test::free_all_residues(add.dst);
}

std::set<fs::path> entries_under(const fs::path &dir) {
    std::set<fs::path> entries;
    for (const auto &entry : fs::directory_iterator(dir))
        entries.insert(entry.path());
    return entries;
}

fs::path this_executable() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    REQUIRE(_NSGetExecutablePath(path.data(), &size) == 0);
    return fs::canonical(path.c_str());
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}

char **process_environment() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

// Runs this binary's hidden case `filter` in its own process, with the caller's
// environment plus the sibling's program directory, and returns its raw wait
// status: 0 exactly when it exited normally with status 0. Its stdout goes to
// this process's stderr, so it cannot interleave with this process's reporter.
int run_sibling(const fs::path &dir, const char *filter) {
    std::string self = this_executable().string();
    std::string filter_arg = filter;
    std::string variable = std::string(kSiblingDirVariable) + "=" + dir.string();
    std::vector<char *> env;
    for (char **e = process_environment(); *e != nullptr; ++e)
        env.push_back(*e);
    env.push_back(variable.data());
    env.push_back(nullptr);
    std::vector<char *> argv = {self.data(), filter_arg.data(), nullptr};
    posix_spawn_file_actions_t actions;
    REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
    const int dup_result = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    pid_t pid = 0;
    const int spawn_result = dup_result == 0 ? posix_spawn(&pid, self.c_str(), &actions, nullptr,
                                                           argv.data(), env.data())
                                             : dup_result;
    posix_spawn_file_actions_destroy(&actions);
    REQUIRE(dup_result == 0);
    REQUIRE(spawn_result == 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    return status;
}

} // namespace

// Hidden: run_sibling() starts it in a separate process.
TEST_CASE("sibling process tags an input and resets", "[.sibling]") {
    const char *dir = std::getenv(kSiblingDirVariable);
    REQUIRE(dir != nullptr);
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    configure(dir, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xB000ULL), base);
    haze::test::free_all_residues(src);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// Hidden: run_sibling() starts it in a separate process, which exits with an
// input still tagged and no reset, as a killed run would.
TEST_CASE("sibling process tags an input and exits without a reset", "[.crashing_sibling]") {
    const char *dir = std::getenv(kSiblingDirVariable);
    REQUIRE(dir != nullptr);
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    configure(dir, base);
    [[maybe_unused]] const auto src =
        haze::test::allocate_and_h2d_residues(residues_for(base, 0xC000ULL), base);
    std::_Exit(EXIT_SUCCESS);
}

TEST_CASE("a sibling process's reset keeps this process's pending inputs", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const fs::path root = fs::absolute("haze_spill_sibling");
    fs::remove_all(root);
    configure(root / "session_this", base);
    const PendingAdd add = record_add(base, 0xA000ULL);

    REQUIRE(run_sibling(root / "session_sibling", kResettingSiblingFilter) == 0);

    flush_and_check(add, base);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(root);
}

TEST_CASE("the next reset under a program dir removes a killed run's spill files",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const fs::path root = fs::absolute("haze_spill_killed");
    fs::remove_all(root);
    const fs::path dir = root / "session";

    REQUIRE(run_sibling(dir, kCrashingSiblingFilter) == 0);
    // The dead run left its program dir and a non-empty spill root beside it.
    const fs::path spill_root = haze::InputSpillStore::root_for(dir);
    REQUIRE(entries_under(root) == std::set<fs::path>{dir, spill_root});
    REQUIRE_FALSE(fs::is_empty(spill_root));

    configure(dir, base);
    flush_and_check(record_add(base, 0xD000ULL), base);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(entries_under(root) == std::set<fs::path>{dir});
    fs::remove_all(root);
}

TEST_CASE("a relative program dir roots the spill store beside the dir it names", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const fs::path root = fs::absolute("haze_spill_relative");
    fs::remove_all(root);
    // The symlink layout of test_program_directory_cleanup.cpp: shallow/link -> deep/target,
    // so shallow/link/.. is physically deep/, while lexically it is shallow/.
    const fs::path deep = root / "deep";
    const fs::path shallow = root / "shallow";
    REQUIRE(fs::create_directories(deep / "target"));
    REQUIRE(fs::create_directories(shallow));
    REQUIRE(fs::create_directories(root / "sub"));
    std::error_code link_ec;
    fs::create_directory_symlink(deep / "target", shallow / "link", link_ec);
    REQUIRE_FALSE(link_ec);

    // Relative, with a plain '..', a '..' after the symlink and a trailing separator; the
    // vendor compiler's own file opens resolve it to deep/session.
    configure(fs::path{"haze_spill_relative"} / "sub" / ".." / "shallow" / "link" / ".." /
                  "session" / "",
              base);
    const PendingAdd add = record_add(base, 0xE000ULL);
    const fs::path dir = deep / "session";
    REQUIRE(entries_under(deep) ==
            std::set<fs::path>{deep / "target", dir, haze::InputSpillStore::root_for(dir)});
    REQUIRE(entries_under(shallow) == std::set<fs::path>{shallow / "link"});
    flush_and_check(add, base);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(root);
}
