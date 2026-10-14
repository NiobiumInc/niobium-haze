// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "filesystem_helpers.hpp"
#include "mod_arith_ref.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string>
#include <system_error>
#include <vector>

// HAZE takes the whole configuration in one hazeConfigureDevice call
// (hazeFheParams + optional hazeReplayConfig) — the caller owns the structs and
// no config state is held before the call. There are no getters, so accept
// tests can only assert HAZE_SUCCESS. Per-argument problems (a bad ring
// dimension, a zero modulus, too many moduli, an NTT root that is not a
// primitive 2N-th root) surface as HAZE_ERROR_INVALID_VALUE; whole-config
// invariants (duplicate moduli, reconfiguring under live allocations) as
// HAZE_ERROR_CONFIGERR. Nothing is installed on a failed configure.

namespace {

namespace ma = niobium::mod_arith;

constexpr uint64_t kRingDim = 4096;
// Two distinct NTT-friendly primes (q ≡ 1 mod 2N) for N=4096; the same
// constants are used by test_basis_convert.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;

} // namespace

TEST_CASE("hazeConfigureDevice accepts a well-formed config", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const uint64_t twiddle[] = {ma::qnr_root(kQ0, kRingDim), ma::qnr_root(kQ1, kRingDim)};
    const hazeFheParams fhe = {.ring_dim = kRingDim,
                               .moduli = moduli,
                               .moduli_count = 2,
                               .twiddle_generators = twiddle,
                               .twiddle_count = 2};
    const hazeReplayConfig replay = {.target = "FHE_SIM",
                                     .program_name = "my-program",
                                     .program_version = "1.2.3",
                                     .program_description = "experimental run"};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    // No public getters; HAZE_SUCCESS is the only assertion. The "rejects ..."
    // cases below cover the validation gates.
}

TEST_CASE("hazeConfigureDevice accepts every supported ring dimension", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    for (uint64_t n : {4096ULL, 8192ULL, 16384ULL, 32768ULL, 65536ULL}) {
        const hazeFheParams fhe = {.ring_dim = n};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    }
}

TEST_CASE("hazeConfigureDevice defaults the replay config when null", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects a null hazeFheParams", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects a null moduli array with a non-zero count", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096, .moduli = nullptr, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects unsupported ring dimensions", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // 0 (unset), and non-power-of-two / out-of-envelope values.
    for (uint64_t n : {0ULL, 100ULL, 3000ULL, 5000ULL}) {
        const hazeFheParams fhe = {.ring_dim = n};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    }
}

TEST_CASE("hazeConfigureDevice rejects a zero modulus", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {12289, 0};
    const hazeFheParams fhe = {.ring_dim = 4096, .moduli = moduli, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects too many ciphertext moduli", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // One past the device envelope (hazeDeviceProp::maxCiphertextModuli == 64);
    // all distinct and non-zero, so only the count trips the rejection.
    std::vector<uint64_t> moduli(65);
    for (std::size_t i = 0; i < moduli.size(); ++i)
        moduli[i] = 12289 + (i * 2);
    const hazeFheParams fhe = {
        .ring_dim = 4096, .moduli = moduli.data(), .moduli_count = moduli.size()};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("hazeConfigureDevice rejects duplicate ciphertext moduli", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t dup[] = {12289, 12289};
    const hazeFheParams bad = {.ring_dim = 4096, .moduli = dup, .moduli_count = 2};
    // Each modulus is per-slot valid but collides on the whole-config uniqueness
    // check, which lives in build().
    REQUIRE(hazeConfigureDevice(&bad, nullptr) == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
    // Nothing is installed on failure, so a corrected config configures cleanly.
    const uint64_t ok[] = {12289, 40961};
    const hazeFheParams good = {.ring_dim = 4096, .moduli = ok, .moduli_count = 2};
    REQUIRE(hazeConfigureDevice(&good, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeDeviceReset re-permits configuration", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // After reset a fresh configure still succeeds.
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects an out-of-range opt_level", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    for (const int bad : {-1, 4, 100}) {
        const hazeReplayConfig replay = {.opt_level = bad};
        REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        const std::string message = hazeGetLastErrorMessage();
        REQUIRE_FALSE(message.empty());
        REQUIRE(message.find("opt_level") != std::string::npos);
    }
    // Nothing is installed on failure: a corrected config still configures
    // cleanly afterward.
    const hazeReplayConfig ok = {.opt_level = 3};
    REQUIRE(hazeConfigureDevice(&fhe, &ok) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects a remove_program_directory value other than 0 or 1",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    for (const int bad : {-1, 2, 100}) {
        const hazeReplayConfig replay = {.remove_program_directory = bad};
        REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        const std::string message = hazeGetLastErrorMessage();
        REQUIRE_FALSE(message.empty());
        REQUIRE(message.find("remove_program_directory") != std::string::npos);
    }
    // Nothing is installed on failure: a corrected config still configures
    // cleanly afterward. 0, not 1, because 1 has its own further
    // requirements (see the "rejects remove_program_directory ..." cases
    // below) that are out of scope for this range check.
    const hazeReplayConfig ok = {.remove_program_directory = 0};
    REQUIRE(hazeConfigureDevice(&fhe, &ok) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects remove_program_directory without an explicit "
          "program_directory",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const hazeReplayConfig replay = {.remove_program_directory = 1}; // program_directory NULL
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);
}

TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory that is "
          "the current directory",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    // "." always resolves to the current directory, whatever it is; the
    // rejection does not depend on this process's specific cwd.
    const hazeReplayConfig replay = {.program_directory = ".", .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);
}

TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory that is a "
          "filesystem root or an ancestor of the current directory",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    // "/" is a root outright; ".." and the cwd's own parent's absolute path
    // are both ancestors of the current directory (the ctest working
    // directory is never "/" itself, so ".." names a real ancestor here).
    const std::string parent = std::filesystem::current_path().parent_path().string();
    for (const char *bad : {"/", "..", parent.c_str()}) {
        const hazeReplayConfig replay = {.program_directory = bad, .remove_program_directory = 1};
        REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        const std::string message = hazeGetLastErrorMessage();
        REQUIRE_FALSE(message.empty());
        REQUIRE(message.find("program_directory") != std::string::npos);
    }
}

TEST_CASE("hazeConfigureDevice names an unreadable current directory when "
          "remove_program_directory is set",
          "[unit]") {
    const haze::test::RestoreCwd restore_cwd;
    const std::filesystem::path program_dir =
        haze::test::make_unique_temp_dir("haze_unreadable_cwd_program");
    const std::filesystem::path removed = haze::test::make_unique_temp_dir("haze_unreadable_cwd");
    std::filesystem::current_path(removed);
    std::filesystem::remove(removed);
    std::error_code cwd_ec;
    REQUIRE(std::filesystem::current_path(cwd_ec).empty());
    REQUIRE(cwd_ec);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const std::string dir = program_dir.string();
    const hazeReplayConfig replay = {.program_directory = dir.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE(message.find("the current directory cannot be read") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(program_dir, ec);
}

// N1 regression guard: the ancestor/cwd check must resolve program_directory
// PHYSICALLY (following symlinks), not lexically. A symlink can make a path
// that is LEXICALLY unrelated to the current directory or any of its
// ancestors resolve PHYSICALLY to exactly one of them.
TEST_CASE("hazeConfigureDevice rejects a symlinked program_directory that physically resolves "
          "to an ancestor of the current directory",
          "[unit]") {
    const haze::test::RestoreCwd restore_cwd;
    const std::filesystem::path scratch =
        haze::test::make_unique_temp_dir("haze_n1_symlink_ancestor");
    std::filesystem::current_path(scratch);

    // holder/alias is a symlink to scratch's own parent (fs::temp_directory_path(),
    // an ancestor of the new current directory, `scratch`) -- lexically
    // unrelated to `scratch` or any of its ancestors, but physically IS one.
    const std::filesystem::path holder =
        haze::test::make_unique_temp_dir("haze_n1_symlink_ancestor_holder");
    std::error_code link_ec;
    std::filesystem::create_directory_symlink(scratch.parent_path(), holder / "alias", link_ec);
    REQUIRE_FALSE(link_ec);

    const hazeFheParams fhe = {.ring_dim = 4096};
    const std::string alias_path = (holder / "alias").string();
    const hazeReplayConfig replay = {.program_directory = alias_path.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(holder, ec);
    std::filesystem::remove_all(scratch, ec);
}

// N1 regression guard (kept from an earlier round; now rejected via a
// different, simpler mechanism). "missing" doesn't exist, so
// std::filesystem::canonical() fails outright and this is rejected as
// "must already exist as a directory" -- the earlier weakly_canonical-based
// check instead fell back to lexical handling for the whole "missing/.."
// suffix, which could (and once did) let a path that names the current
// directory itself slip through as though it were unrelated.
TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory naming a "
          "missing directory followed by '..'",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const hazeReplayConfig replay = {.program_directory = "missing/..",
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);
}

// Same shape, one level up: the missing component sits under an ancestor of
// the current directory rather than the current directory itself.
TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory naming a "
          "missing directory under an ancestor, followed by '..'",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const std::string bad =
        (std::filesystem::current_path().parent_path() / "missing" / "..").string();
    const hazeReplayConfig replay = {.program_directory = bad.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);
}

// N1 regression guard, round 4: weakly_canonical's lexical fallback for a
// missing component doesn't just mishandle the ".." right after it -- it
// gives up on filesystem resolution for the ENTIRE remaining suffix, so a
// REAL symlink further down that suffix never gets followed during
// validation even though the compiler's own (real) file opens do follow
// it. "uplink" is a genuine symlink back to the parent of the current
// directory; "missing/../uplink/<cwd's own name>" -- lexically, after
// "missing/.." cancels, this is just "uplink/<cwd's own name>", never
// resolved as a symlink under the old check -- physically names the
// current directory itself (uplink's target's child named the same as the
// current directory). canonical() closes this: it fails outright the
// moment it reaches "missing", before any lexical fallback ever has a
// chance to skip past "uplink" unresolved.
TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory naming a "
          "missing component followed by a symlink back to the current directory",
          "[unit]") {
    const haze::test::RestoreCwd restore_cwd;
    const std::filesystem::path root =
        haze::test::make_unique_temp_dir("haze_n1_missing_then_symlink");
    const std::filesystem::path cwd_dir = root / "cwd";
    REQUIRE(std::filesystem::create_directories(cwd_dir));
    std::error_code link_ec;
    // uplink -> ".." : from inside cwd_dir, "uplink" resolves to `root`.
    std::filesystem::create_directory_symlink("..", cwd_dir / "uplink", link_ec);
    REQUIRE_FALSE(link_ec);
    std::filesystem::current_path(cwd_dir);

    const hazeFheParams fhe = {.ring_dim = 4096};
    // "missing" doesn't exist; "uplink" does, and points back to `root`, so
    // "uplink/cwd" would physically be `root / "cwd"` == cwd_dir itself.
    const hazeReplayConfig replay = {.program_directory = "missing/../uplink/cwd",
                                     .remove_program_directory = 1};
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// N1 round 4: remove_program_directory now requires program_directory to
// already exist as a directory at configure time -- a plain, no-tricks
// nonexistent path must be rejected on that basis alone.
TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory that does "
          "not exist",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const std::filesystem::path missing =
        haze::test::make_unique_temp_dir("haze_n1_missing_dir_holder") / "does_not_exist";
    const std::string missing_str = missing.string();
    const hazeReplayConfig replay = {.program_directory = missing_str.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(missing.parent_path(), ec);
}

// N1 round 4: a dangling symlink (target doesn't exist) must be rejected
// the same way as a plain missing path -- canonical() requires the fully
// resolved destination to exist, and a dangling symlink's doesn't.
TEST_CASE("hazeConfigureDevice rejects a remove_program_directory program_directory that is a "
          "dangling symlink",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const std::filesystem::path holder =
        haze::test::make_unique_temp_dir("haze_n1_dangling_symlink");
    std::error_code link_ec;
    std::filesystem::create_directory_symlink(holder / "nonexistent_target", holder / "dangling",
                                              link_ec);
    REQUIRE_FALSE(link_ec);

    const hazeFheParams fhe = {.ring_dim = 4096};
    const std::string dangling_str = (holder / "dangling").string();
    const hazeReplayConfig replay = {.program_directory = dangling_str.c_str(),
                                     .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(holder, ec);
}

// N4 regression guard: an empty program_directory must not reach a
// filesystem call at all (a prior draft passed "" straight into the
// throwing absolute() overload inside this noexcept function, aborting the
// process -- exit 134/SIGABRT -- instead of returning an error).
TEST_CASE("hazeConfigureDevice rejects an empty remove_program_directory program_directory",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = 4096};
    const hazeReplayConfig replay = {.program_directory = "", .remove_program_directory = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    const std::string message = hazeGetLastErrorMessage();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("program_directory") != std::string::npos);
}
TEST_CASE("hazeConfigureDevice rejects an NTT root that is not a primitive 2N-th root", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const uint64_t psi0 = ma::qnr_root(kQ0, kRingDim);
    const uint64_t psi1 = ma::qnr_root(kQ1, kRingDim);
    // Each candidate replaces the second root, so the error must name index 1.
    const uint64_t bad_roots[] = {
        0,                           // never a root
        1,                           // order 1
        kQ1 - 1,                     // -1: order 2
        ma::mulmod(psi1, psi1, kQ1), // psi^2: order N, not 2N
        psi1 + kQ1,                  // a root, but not in [1, q)
        psi0,                        // a root for the other modulus
        ma::smallest_qnr(kQ1),       // a non-residue, not a root
    };
    for (const uint64_t bad : bad_roots) {
        CAPTURE(bad);
        const uint64_t roots[] = {psi0, bad};
        const hazeFheParams fhe = {.ring_dim = kRingDim,
                                   .moduli = moduli,
                                   .moduli_count = 2,
                                   .twiddle_generators = roots,
                                   .twiddle_count = 2};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        const std::string message = hazeGetLastErrorMessage();
        CAPTURE(message);
        REQUIRE(message.find("twiddle_generators[1]") != std::string::npos);
        REQUIRE(message.find(std::to_string(kQ1)) != std::string::npos);
    }
    // Nothing is installed on failure: the corrected roots configure cleanly.
    const uint64_t good[] = {psi0, psi1};
    const hazeFheParams fhe = {.ring_dim = kRingDim,
                               .moduli = moduli,
                               .moduli_count = 2,
                               .twiddle_generators = good,
                               .twiddle_count = 2};
    REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_SUCCESS);
}

TEST_CASE("hazeConfigureDevice rejects a twiddle_count other than 0 or moduli_count", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t moduli[] = {kQ0, kQ1};
    const uint64_t psi0 = ma::qnr_root(kQ0, kRingDim);
    const uint64_t psi1 = ma::qnr_root(kQ1, kRingDim);
    const uint64_t roots[] = {psi0, psi1, psi1};
    struct Shape {
        std::size_t moduli_count;
        std::size_t twiddle_count;
    };
    for (const Shape shape : {Shape{.moduli_count = 2, .twiddle_count = 1},
                              Shape{.moduli_count = 2, .twiddle_count = 3},
                              Shape{.moduli_count = 0, .twiddle_count = 1}}) {
        CAPTURE(shape.moduli_count, shape.twiddle_count);
        const hazeFheParams fhe = {.ring_dim = kRingDim,
                                   .moduli = moduli,
                                   .moduli_count = shape.moduli_count,
                                   .twiddle_generators = roots,
                                   .twiddle_count = shape.twiddle_count};
        REQUIRE(hazeConfigureDevice(&fhe, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        const std::string message = hazeGetLastErrorMessage();
        CAPTURE(message);
        REQUIRE(message.find("twiddle_count") != std::string::npos);
    }
}
