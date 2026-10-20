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
// hazeMemcpyMrpFormat: the polynomial form an upload declares is the form its
// recorded input .bin carries, through cross-epoch reuse.

#include "cereal/archives/portable_binary.hpp"
#include "integration_helpers.hpp"
#include "openfhe.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
const std::vector<uint64_t> kBase = {kQ0, kQ1};

// Reset, then configure `program_name` (relative to the runs dir) on `target`.
fs::path configure(const char *target, const std::string &program_name) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const fs::path dir{program_name};
    fs::remove_all(dir);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = kBase.data(), .moduli_count = kBase.size()};
    const hazeReplayConfig replay = {
        .target = target, .program_name = program_name.c_str(), .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0) == HAZE_SUCCESS);
    return dir;
}

std::vector<std::vector<uint64_t>> residues(uint64_t seed) {
    std::vector<std::vector<uint64_t>> r;
    r.reserve(kBase.size());
    for (std::size_t i = 0; i < kBase.size(); ++i)
        r.push_back(haze::test::make_residue(kBase[i], seed + i, kRingDim));
    return r;
}

std::vector<const void *> host_views(const std::vector<std::vector<uint64_t>> &values) {
    std::vector<const void *> views;
    views.reserve(values.size());
    for (const auto &v : values)
        views.push_back(v.data());
    return views;
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

std::string slurp(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every DCRTPoly in a CIPHERTEXT input .bin, and every tower of each, is in `expected`.
// Layout: write_ciphertext_input_files in niobium-fhetch's src/auto_facade.cpp.
void require_bin_format(const fs::path &bin, Format expected) {
    INFO(bin.string());
    std::ifstream in(bin, std::ios::binary);
    REQUIRE(in.good());
    cereal::PortableBinaryInputArchive ar(in);
    uint32_t instances = 0;
    uint8_t payload_type = 0;
    uint32_t count = 0;
    ar(instances, payload_type, count);
    REQUIRE(instances == 1);
    REQUIRE(payload_type == 0);
    REQUIRE(count > 0);
    for (uint32_t i = 0; i < count; ++i) {
        lbcrypto::DCRTPoly dcrt;
        ar(dcrt);
        REQUIRE(dcrt.GetFormat() == expected);
        REQUIRE_FALSE(dcrt.GetAllElements().empty());
        for (const auto &tower : dcrt.GetAllElements())
            REQUIRE(tower.GetFormat() == expected);
    }
}

} // namespace

TEST_CASE("upload format: a coefficient-tagged upload records COEFFICIENT polynomials",
          "[integration]") {
    const fs::path dir = configure("FUNC_SIM", "haze_upload_format_coefficient");
    const auto src = haze::test::allocate_and_h2d_residues(residues(0xF100ULL), kBase,
                                                           HAZE_POLY_FORMAT_COEFFICIENT);
    const auto out = record_ntt(src);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    require_bin_format(input_file(dir, "haze_mrp_in_0", ".bin"), Format::COEFFICIENT);
    haze::test::free_all_residues(src);
    haze::test::free_all_residues(out);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

TEST_CASE("upload format: hazeMemcpyMrp records what an EVALUATION-tagged upload records",
          "[integration]") {
    std::vector<std::string> bins;
    std::vector<std::string> ids;
    for (const auto format :
         {std::optional<hazePolyFormat>{}, std::optional{HAZE_POLY_FORMAT_EVALUATION}}) {
        const fs::path dir = configure("FUNC_SIM", "haze_upload_format_evaluation");
        const auto src = haze::test::allocate_and_h2d_residues(residues(0xF200ULL), kBase, format);
        const auto out = record_ntt(src);
        REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
        const fs::path bin = input_file(dir, "haze_mrp_in_0", ".bin");
        require_bin_format(bin, Format::EVALUATION);
        bins.push_back(slurp(bin));
        ids.push_back(slurp(input_file(dir, "haze_mrp_in_0", ".ids")));
        haze::test::free_all_residues(src);
        haze::test::free_all_residues(out);
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
        fs::remove_all(dir);
    }
    REQUIRE(bins[0] == bins[1]);
    REQUIRE(ids[0] == ids[1]);
}

TEST_CASE("upload format: hazeMemcpyMrpFormat rejects an unknown form and a non-H2D coefficient",
          "[integration]") {
    const fs::path dir = configure("FUNC_SIM", "haze_upload_format_rejects");
    const auto values = residues(0xF300ULL);
    const auto src =
        haze::test::allocate_and_h2d_residues(values, kBase, HAZE_POLY_FORMAT_EVALUATION);
    const auto dst = haze::test::allocate_dst_residues(kBase.size(), kBytes);
    const std::vector<const void *> host_src = host_views(values);
    std::vector<std::vector<uint64_t>> host_dst(kBase.size(), std::vector<uint64_t>(kRingDim));
    std::vector<void *> host_dst_ptrs;
    host_dst_ptrs.reserve(host_dst.size());
    for (auto &v : host_dst)
        host_dst_ptrs.push_back(v.data());
    const std::vector<const void *> src_view = haze::test::to_const(src);

    SECTION("a value outside hazePolyFormat") {
        // memcpy, not static_cast: an out-of-range enum constant trips -Wconversion.
        const int raw = 2;
        hazePolyFormat unknown{};
        static_assert(sizeof(unknown) == sizeof(raw));
        std::memcpy(&unknown, &raw, sizeof(unknown));
        REQUIRE(hazeMemcpyMrpFormat(dst.data(), host_src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE,
                                    kBase.data(), kBase.size(),
                                    unknown) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        char name[64];
        REQUIRE(hazeInputGroupName(dst[0], name, sizeof name) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    }
    SECTION("COEFFICIENT with D2H") {
        REQUIRE(hazeMemcpyMrpFormat(host_dst_ptrs.data(), src_view.data(), kBytes,
                                    HAZE_MEMCPY_DEVICE_TO_HOST, kBase.data(), kBase.size(),
                                    HAZE_POLY_FORMAT_COEFFICIENT) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeMemcpyMrpFormat(host_dst_ptrs.data(), src_view.data(), kBytes,
                                    HAZE_MEMCPY_DEVICE_TO_HOST, kBase.data(), kBase.size(),
                                    HAZE_POLY_FORMAT_EVALUATION) == HAZE_SUCCESS);
        REQUIRE(host_dst == values);
    }
    SECTION("COEFFICIENT with D2D") {
        REQUIRE(hazeMemcpyMrpFormat(dst.data(), src_view.data(), kBytes,
                                    HAZE_MEMCPY_DEVICE_TO_DEVICE, kBase.data(), kBase.size(),
                                    HAZE_POLY_FORMAT_COEFFICIENT) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeMemcpyMrpFormat(dst.data(), src_view.data(), kBytes,
                                    HAZE_MEMCPY_DEVICE_TO_DEVICE, kBase.data(), kBase.size(),
                                    HAZE_POLY_FORMAT_EVALUATION) == HAZE_SUCCESS);
    }

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

TEST_CASE("upload format: cross-epoch reuse of a coefficient upload keeps its form",
          "[integration]") {
    const fs::path dir = configure("local", "haze_upload_format_reuse");
    const auto src = haze::test::allocate_and_h2d_residues(residues(0xF400ULL), kBase,
                                                           HAZE_POLY_FORMAT_COEFFICIENT);
    const std::vector<const void *> src_view = haze::test::to_const(src);
    const auto add = [&src_view]() {
        const auto dst = haze::test::allocate_dst_residues(kBase.size(), kBytes);
        REQUIRE(hazeAddMrp(dst.data(), src_view.data(), src_view.data(), kBase.data(), kBase.size(),
                           nullptr) == HAZE_SUCCESS);
        for (void *p : dst)
            REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
        return dst;
    };
    const auto first_sum = add();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // haze_in_0/1 bind the uploaded residues again from their spill records.
    const auto reused_sum = add();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (const char *input : {"haze_in_0", "haze_in_1"})
        require_bin_format(input_file(dir, input, ".bin"), Format::COEFFICIENT);

    for (const auto *group : {&src, &first_sum, &reused_sum})
        haze::test::free_all_residues(*group);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}
