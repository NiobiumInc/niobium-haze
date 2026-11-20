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
// hazeSetOutputMetadata: what a tagged output is decoded with reaches the written program's
// ciphertext template for that output, goes with the pending outputs at finalize, and is refused
// outside its ranges or for an address that is not a pending output.

#include "ciphertext-ser.h"
#include "integration_helpers.hpp"
#include "openfhe.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
const std::vector<uint64_t> kBase = {kQ0, kQ1};
constexpr hazeOutputMetadata kMetadata = {
    .scaling_factor = 1099511627776.0, .noise_scale_deg = 2, .slots = 8};

// Reset, then configure `program_name` (relative to the runs dir); the program is only written.
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

struct Recorded {
    std::vector<void *> src;
    std::vector<void *> out;
};

// Records hazeNTTMrp over a declared input into fresh slots and tags them.
Recorded record_ntt() {
    Recorded r{.src = haze::test::allocate_dst_residues(kBase.size(), kBytes),
               .out = haze::test::allocate_dst_residues(kBase.size(), kBytes)};
    REQUIRE(hazeDeclareMrp(r.src.data(), kBytes, kBase.data(), kBase.size()) == HAZE_SUCCESS);
    const std::vector<const void *> view = haze::test::to_const(r.src);
    REQUIRE(hazeNTTMrp(r.out.data(), view.data(), kBase.data(), kBase.size(), nullptr) ==
            HAZE_SUCCESS);
    for (void *p : r.out)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
    return r;
}

std::string output_group_name(const void *ptr) {
    char name[64] = {};
    REQUIRE(hazeOutputGroupName(ptr, name, sizeof name) == HAZE_SUCCESS);
    return name;
}

lbcrypto::Ciphertext<lbcrypto::DCRTPoly> read_template(const fs::path &dir,
                                                       const std::string &name) {
    const fs::path path = dir / "ciphertext_templates" / (name + ".template");
    REQUIRE(fs::exists(path));
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ct;
    REQUIRE(lbcrypto::Serial::DeserializeFromFile(path.string(), ct, lbcrypto::SerType::BINARY));
    REQUIRE(ct != nullptr);
    return ct;
}

void finish(const Recorded &r, const fs::path &dir) {
    haze::test::free_all_residues(r.src);
    haze::test::free_all_residues(r.out);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(dir);
}

} // namespace

TEST_CASE("output metadata: the written template carries it", "[integration]") {
    const fs::path dir = configure("haze_output_metadata_template");
    const Recorded r = record_ntt();
    const std::string name = output_group_name(r.out[0]);
    // Any residue names the whole output; the last call wins.
    REQUIRE(hazeSetOutputMetadata(r.out[0], &kMetadata) == HAZE_SUCCESS);
    hazeOutputMetadata replaced = kMetadata;
    replaced.slots = 16;
    REQUIRE(hazeSetOutputMetadata(r.out[1], &replaced) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto ct = read_template(dir, name);
    REQUIRE(ct->GetScalingFactor() == replaced.scaling_factor);
    REQUIRE(ct->GetNoiseScaleDeg() == replaced.noise_scale_deg);
    REQUIRE(ct->GetSlots() == replaced.slots);
    finish(r, dir);
}

TEST_CASE("output metadata: goes with the pending outputs at finalize", "[integration]") {
    const fs::path dir = configure("haze_output_metadata_finalize");
    const Recorded first = record_ntt();
    REQUIRE(hazeSetOutputMetadata(first.out[0], &kMetadata) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    // The finalize consumed the outputs: there is no output left to describe.
    REQUIRE(hazeSetOutputMetadata(first.out[0], &kMetadata) == HAZE_ERROR_SOURCE_UNAVAILABLE);

    // The next epoch's output goes by the same name, and gets nothing from the first one.
    const Recorded second = record_ntt();
    const std::string name = output_group_name(second.out[0]);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    const auto ct = read_template(dir, name);
    REQUIRE(ct->GetSlots() != kMetadata.slots);
    REQUIRE(ct->GetScalingFactor() != kMetadata.scaling_factor);
    haze::test::free_all_residues(first.src);
    haze::test::free_all_residues(first.out);
    finish(second, dir);
}

TEST_CASE("output metadata: the refusals", "[integration]") {
    const fs::path dir = configure("haze_output_metadata_rejects");
    const Recorded r = record_ntt();
    hazeOutputMetadata bad = kMetadata;

    SECTION("null arguments") {
        REQUIRE(hazeSetOutputMetadata(nullptr, &kMetadata) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeSetOutputMetadata(r.out[0], nullptr) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("a scaling factor that is not finite and positive") {
        bad.scaling_factor = 0.0;
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
        bad.scaling_factor = std::numeric_limits<double>::infinity();
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("a scale degree of zero") {
        bad.noise_scale_deg = 0;
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("slots that are no power of two, or more than half the ring") {
        bad.slots = 0;
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
        bad.slots = 12;
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
        bad.slots = kRingDim;
        REQUIRE(hazeSetOutputMetadata(r.out[0], &bad) == HAZE_ERROR_INVALID_VALUE);
    }
    SECTION("an input is no pending output") {
        REQUIRE(hazeSetOutputMetadata(r.src[0], &kMetadata) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    }
    finish(r, dir);
}
