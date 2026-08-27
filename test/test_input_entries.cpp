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
// On-disk shape of a recorded project's inputs. Nothing else in the tree reads
// <prog>.inputs.json or the .ids files it names, which is how each ciphertext
// residue came to be recorded twice - once as a modulus-less haze_in_<n> and
// again inside a haze_mrp_in_<m>. These assert the invariant that replaced it:
// every address is bound by exactly one entry, the one its upload declared.

#include "integration_helpers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <ios>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

// One recorded input as the manifest describes it.
struct InputEntry {
    std::string name;
    std::string ids_file;
    std::string bin_file;
};

std::string slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Pull "<key>": "<value>" occurrences in document order. A string scan keeps the
// test free of a JSON library, matching test_is_half_modulus.cpp's json_value_text;
// the manifest's values are plain names and filenames, never escaped.
std::vector<std::string> json_string_values(const std::string &doc, const std::string &key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\"";
    for (std::size_t pos = doc.find(needle); pos != std::string::npos;
         pos = doc.find(needle, pos + 1)) {
        const std::size_t colon = doc.find(':', pos + needle.size());
        if (colon == std::string::npos)
            break;
        const std::size_t open = doc.find('"', colon);
        if (open == std::string::npos)
            break;
        const std::size_t close = doc.find('"', open + 1);
        if (close == std::string::npos)
            break;
        out.push_back(doc.substr(open + 1, close - open - 1));
    }
    return out;
}

std::vector<InputEntry> read_manifest(const std::filesystem::path &dir,
                                      const std::string &program_name) {
    const std::string doc = slurp(dir / (program_name + ".inputs.json"));
    const auto names = json_string_values(doc, "name");
    const auto ids = json_string_values(doc, "ids_file");
    const auto bins = json_string_values(doc, "bin_file");
    REQUIRE(names.size() == ids.size());
    REQUIRE(names.size() == bins.size());
    std::vector<InputEntry> out;
    out.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        out.push_back({names[i], ids[i], bins[i]});
    return out;
}

// .ids layout (cereal_io.h): [uint64 count][uint64 addr_id]*count.
std::vector<uint64_t> read_ids(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    uint64_t count = 0;
    in.read(reinterpret_cast<char *>(&count), sizeof(count));
    REQUIRE(in.good());
    std::vector<uint64_t> ids(count);
    if (count > 0) {
        in.read(reinterpret_cast<char *>(ids.data()),
                static_cast<std::streamsize>(count * sizeof(uint64_t)));
    }
    REQUIRE(in.good());
    return ids;
}

// Configure a uniquely named project. FUNC_SIM keeps this off the in-process
// simulator; hazeWriteProgram finalizes the program dir without replaying, so
// no compiler binary is needed.
void configure(const std::string &program_name, const std::vector<uint64_t> &moduli) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {
        .ring_dim = kRingDim, .moduli = moduli.data(), .moduli_count = moduli.size()};
    const hazeReplayConfig replay = {.target = "FUNC_SIM",
                                     .program_name = program_name.c_str(),
                                     .program_version = "0.1",
                                     .program_description = "input entry shape test",
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    uint64_t scaffold = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli[0], &scaffold) == HAZE_SUCCESS);
}

std::vector<std::vector<uint64_t>> residues_for(const std::vector<uint64_t> &base, uint64_t seed) {
    std::vector<std::vector<uint64_t>> r(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        r[i] = haze::test::make_residue(base[i], seed + i, kRingDim);
    return r;
}

std::size_t count_prefixed(const std::vector<InputEntry> &entries, const std::string &prefix) {
    std::size_t n = 0;
    for (const auto &e : entries) {
        if (e.name.starts_with(prefix))
            ++n;
    }
    return n;
}

// Record `src + src` over one uploaded MRP group and return the project dir.
std::filesystem::path record_one_group(const std::string &program_name,
                                       const std::vector<uint64_t> &base) {
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x5150ULL), base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    return std::filesystem::path{program_name};
}

} // namespace

TEST_CASE("recorded inputs: every address is bound by exactly one entry", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const auto dir = record_one_group("haze_inputs_unique", base);
    const auto entries = read_manifest(dir, "haze_inputs_unique");
    REQUIRE_FALSE(entries.empty());

    std::set<uint64_t> seen;
    std::size_t total = 0;
    for (const auto &e : entries) {
        for (uint64_t id : read_ids(dir / e.ids_file)) {
            INFO("address " << id << " claimed again by entry '" << e.name << "'");
            REQUIRE(seen.insert(id).second);
            ++total;
        }
    }
    // Not vacuous: the group's residues really are present.
    REQUIRE(total == base.size());
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: every manifest entry has its .bin and .ids", "[integration]") {
    // load_source_inputs hard-fails on a manifest entry whose files are absent,
    // so a missing pair is an unreplayable project.
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const auto dir = record_one_group("haze_inputs_present", base);
    const auto entries = read_manifest(dir, "haze_inputs_present");
    REQUIRE_FALSE(entries.empty());
    for (const auto &e : entries) {
        INFO("entry '" << e.name << "'");
        REQUIRE(std::filesystem::exists(dir / e.ids_file));
        REQUIRE(std::filesystem::exists(dir / e.bin_file));
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: an MRP upload emits one group entry, not one per residue",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const auto dir = record_one_group("haze_inputs_grouped", base);
    const auto entries = read_manifest(dir, "haze_inputs_grouped");

    REQUIRE(count_prefixed(entries, "haze_mrp_in_") == 1);
    // The pre-fix shape also carried three modulus-less haze_in_<n> entries for
    // exactly these addresses.
    REQUIRE(count_prefixed(entries, "haze_in_") == 0);
    REQUIRE(read_ids(dir / entries.front().ids_file).size() == base.size());
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: a non-grouped single-tower input is still emitted", "[integration]") {
    // Plaintexts, scalars and key material are legitimately single-tower; the
    // plain modulus-less H2D path must keep recording them.
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_inputs_single", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x2020ULL));
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const std::filesystem::path dir{"haze_inputs_single"};
    const auto entries = read_manifest(dir, "haze_inputs_single");
    REQUIRE(count_prefixed(entries, "haze_in_") == 1);
    REQUIRE(count_prefixed(entries, "haze_mrp_in_") == 0);

    haze::test::free_all_residues(src);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: re-uploading the same addresses records both uploads",
          "[integration]") {
    // Each upload builds fresh fhetch polynomials, so the second needs its own
    // entry; a name keyed on the leading addr would drop it and leave the
    // second upload's addresses bound by nothing.
    const std::vector<uint64_t> base = {kQ0, kQ1};
    configure("haze_inputs_reupload", base);

    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x3030ULL), base);
    const auto dst1 = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst1.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    const auto second = residues_for(base, 0x4040ULL);
    std::vector<const void *> hosts(base.size(), nullptr);
    for (std::size_t i = 0; i < base.size(); ++i)
        hosts[i] = second[i].data();
    REQUIRE(hazeMemcpyMrp(src.data(), hosts.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(),
                          base.size()) == HAZE_SUCCESS);

    const auto dst2 = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst2.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst1)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    for (void *out : dst2)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const std::filesystem::path dir{"haze_inputs_reupload"};
    const auto entries = read_manifest(dir, "haze_inputs_reupload");
    REQUIRE(count_prefixed(entries, "haze_mrp_in_") == 2);

    // Disjoint fhetch addresses, so the invariant still holds across both.
    std::set<uint64_t> seen;
    for (const auto &e : entries) {
        for (uint64_t id : read_ids(dir / e.ids_file))
            REQUIRE(seen.insert(id).second);
    }
    REQUIRE(seen.size() == 2 * base.size());

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst1);
    haze::test::free_all_residues(dst2);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: an MRP op refuses a residue uploaded without a modulus",
          "[integration]") {
    // A per-limb hazeMemcpy cannot name the primes, so the group it feeds would
    // be recorded with modulus 0. Refuse at the op rather than record that.
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    configure("haze_inputs_refuse", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x6060ULL));
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);

    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
