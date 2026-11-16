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
// hazeDeclareMrp: an MRP input recorded without its words flushes as an
// input entry with its addresses and an empty word file, under the name an
// upload would get; it takes the checks hazeMemcpyMrp(H2D) makes.
// hazeOutputGroupName: the name a tagged output goes by in the program.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
const std::vector<uint64_t> kBase = {kQ0, kQ1};

// Reset, then configure `program_name` (relative to the runs dir) on FUNC_SIM.
fs::path configure(const std::string &program_name) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const fs::path dir{program_name};
    fs::remove_all(dir);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = kBase.data(), .moduli_count = kBase.size()};
    const hazeReplayConfig replay = {
        .target = "FUNC_SIM", .program_name = program_name.c_str(), .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0) == HAZE_SUCCESS);
    return dir;
}

// Records hazeNTTMrp over `src` into fresh slots and tags them; returns the slots.
std::vector<void *> record_ntt(const std::vector<void *> &src) {
    const std::vector<void *> dst = haze::test::allocate_dst_residues(src.size(), kBytes);
    const std::vector<const void *> view = haze::test::to_const(src);
    REQUIRE(hazeNTTMrp(dst.data(), view.data(), kBase.data(), kBase.size(), nullptr) ==
            HAZE_SUCCESS);
    for (void *p : dst)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
    return dst;
}

fs::path input_file(const fs::path &dir, const std::string &input, const char *extension) {
    return dir / (dir.filename().string() + ".input_" + input + extension);
}

std::string input_group_name(const void *ptr) {
    char name[64] = {};
    REQUIRE(hazeInputGroupName(ptr, name, sizeof name) == HAZE_SUCCESS);
    return name;
}

} // namespace

TEST_CASE("declare mrp: a declared input flushes with its addresses and an empty word file",
          "[integration]") {
    const fs::path dir = configure("haze_declare_mrp_flush");
    const auto declared = haze::test::allocate_dst_residues(kBase.size(), kBytes);
    REQUIRE(hazeDeclareMrp(declared.data(), kBytes, kBase.data(), kBase.size()) == HAZE_SUCCESS);
    // The name an upload of the same group would get.
    REQUIRE(input_group_name(declared[0]) == "haze_mrp_in_0");
    REQUIRE(input_group_name(declared[1]) == "haze_mrp_in_0");

    const auto out = record_ntt(declared);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    const fs::path ids = input_file(dir, "haze_mrp_in_0", ".ids");
    const fs::path bin = input_file(dir, "haze_mrp_in_0", ".bin");
    REQUIRE(fs::exists(ids));
    REQUIRE(fs::file_size(ids) > 0);
    REQUIRE(fs::exists(bin));
    REQUIRE(fs::file_size(bin) == 0);

    haze::test::free_all_residues(declared);
    haze::test::free_all_residues(out);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

TEST_CASE("declare mrp: an uploaded input of the same program carries its words", "[integration]") {
    // The control for the empty word file above: the same recording with an upload.
    const fs::path dir = configure("haze_declare_mrp_control");
    std::vector<std::vector<uint64_t>> values;
    for (std::size_t i = 0; i < kBase.size(); ++i)
        values.push_back(haze::test::make_residue(kBase[i], 0xD0ULL + i, kRingDim));
    const auto uploaded = haze::test::allocate_and_h2d_residues(values, kBase);
    REQUIRE(input_group_name(uploaded[0]) == "haze_mrp_in_0");
    const auto out = record_ntt(uploaded);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    REQUIRE(fs::file_size(input_file(dir, "haze_mrp_in_0", ".bin")) > 0);

    haze::test::free_all_residues(uploaded);
    haze::test::free_all_residues(out);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

TEST_CASE("declare mrp: the checks of hazeMemcpyMrp(H2D)", "[integration]") {
    // hazeDeviceReset at the end frees whatever a section left allocated.
    const fs::path dir = configure("haze_declare_mrp_rejects");
    std::vector<void *> dst = haze::test::allocate_dst_residues(kBase.size(), kBytes);
    char name[64];

    SECTION("null arguments") {
        REQUIRE(hazeDeclareMrp(nullptr, kBytes, kBase.data(), kBase.size()) ==
                HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeDeclareMrp(dst.data(), kBytes, nullptr, kBase.size()) ==
                HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeDeclareMrp(dst.data(), kBytes, kBase.data(), 0) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("an address that is no allocation") {
        REQUIRE(hazeFree(dst[1]) == HAZE_SUCCESS);
        REQUIRE(hazeDeclareMrp(dst.data(), kBytes, kBase.data(), kBase.size()) ==
                HAZE_ERROR_UNKNOWN_ADDRESS);
        REQUIRE(hazeInputGroupName(dst[0], name, sizeof name) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    }
    SECTION("more bytes than a polynomial") {
        REQUIRE(hazeDeclareMrp(dst.data(), kBytes + sizeof(uint64_t), kBase.data(), kBase.size()) ==
                HAZE_ERROR_SIZE_MISMATCH);
        REQUIRE(hazeInputGroupName(dst[0], name, sizeof name) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

TEST_CASE("output group name: a tagged output's name, and the refusals", "[integration]") {
    const fs::path dir = configure("haze_output_group_name");
    const auto src = haze::test::allocate_dst_residues(kBase.size(), kBytes);
    REQUIRE(hazeDeclareMrp(src.data(), kBytes, kBase.data(), kBase.size()) == HAZE_SUCCESS);
    const auto out = record_ntt(src);

    char name[64] = {};
    REQUIRE(hazeOutputGroupName(out[0], name, sizeof name) == HAZE_SUCCESS);
    const std::string first = name;
    REQUIRE(first.starts_with("haze_mrp_out_"));
    REQUIRE(hazeOutputGroupName(out[1], name, sizeof name) == HAZE_SUCCESS);
    REQUIRE(std::string(name) == first);

    // No truncation: a buffer one byte short of the name and its NUL.
    char short_buffer[64] = {};
    REQUIRE(hazeOutputGroupName(out[0], short_buffer, first.size()) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(short_buffer[0] == '\0');
    REQUIRE(hazeOutputGroupName(nullptr, name, sizeof name) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeOutputGroupName(out[0], nullptr, sizeof name) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeOutputGroupName(out[0], name, 0) == HAZE_ERROR_INVALID_VALUE);
    // An input is no pending output.
    REQUIRE(hazeOutputGroupName(src[0], name, sizeof name) != HAZE_SUCCESS);

    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    haze::test::free_all_residues(src);
    haze::test::free_all_residues(out);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}
