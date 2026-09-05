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
// On-disk shape of a recorded project's inputs and outputs. Nothing else in the
// tree reads <prog>.inputs.json or the .ids files it names, which is how each
// ciphertext residue came to be recorded twice - once as a modulus-less
// haze_in_<n> and again inside a haze_mrp_in_<m>, and symmetrically as a
// haze_out_<n> beside its haze_mrp_out_<m>. These assert the invariant that
// replaced it: every address is carried by exactly one entry on each side.

#include "allocator_test_access.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/input_spill.hpp"
#include "integration_helpers.hpp"

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
#include <niobium/fhetch_api.h>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
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

// Output manifest entries carry a name and inline ciphertext_data rather than a
// .bin/.ids pair, so the names alone answer the one-entry-per-residue question.
std::vector<std::string> output_names(const std::filesystem::path &dir,
                                      const std::string &program_name) {
    return json_string_values(slurp(dir / (program_name + ".outputs.json")), "name");
}

std::size_t count_with_prefix(const std::vector<std::string> &names, const std::string &prefix) {
    std::size_t n = 0;
    for (const auto &name : names) {
        if (name.starts_with(prefix))
            ++n;
    }
    return n;
}

std::size_t count_prefixed(const std::vector<InputEntry> &entries, const std::string &prefix) {
    std::size_t n = 0;
    for (const auto &e : entries) {
        if (e.name.starts_with(prefix))
            ++n;
    }
    return n;
}

// Mirrors InputSpillStore's private addr_filename(): the shape is a stable on-disk
// contract a couple of these tests need to check directly. The root sits at cwd
// (a sibling of every program dir, shared for the process's lifetime).
std::filesystem::path spill_file_for(void *ptr) {
    std::ostringstream name;
    name << "addr_" << std::hex << haze::to_uintptr(haze::to_dev_addr(ptr)) << ".spill";
    return std::filesystem::path{"haze_input_spill"} / name.str();
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
    // Zero is the regression target, not an arbitrary bound: before this shape
    // existed the same three addresses ALSO appeared as modulus-less
    // haze_in_<n> entries minted per residue at H2D, which a compute op then
    // duplicated into the group above. Any haze_in_* here means a residue took
    // the undeclared upload path and the duplication has grown back.
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

TEST_CASE("recorded outputs: a grouped output emits one group entry, not one per residue",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const auto dir = record_one_group("haze_outputs_grouped", base);
    const auto names = output_names(dir, "haze_outputs_grouped");

    REQUIRE(count_with_prefix(names, "haze_mrp_out_") == 1);
    // The pre-fix shape also probed each residue individually as haze_out_<n>.
    REQUIRE(count_with_prefix(names, "haze_out_") == 0);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded outputs: a non-grouped output keeps its own probe", "[integration]") {
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_outputs_single", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x7070ULL));
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto names =
        output_names(std::filesystem::path{"haze_outputs_single"}, "haze_outputs_single");
    REQUIRE(count_with_prefix(names, "haze_out_") == 1);
    REQUIRE(count_with_prefix(names, "haze_mrp_out_") == 0);

    haze::test::free_all_residues(src);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded outputs: every residue of a grouped output is still populated",
          "[integration]") {
    // Readback moved to the group's MRP probe, so a missed residue would surface
    // as a stale or unflushed D2H rather than a recording-shape difference.
    const auto base = haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const auto inputs = residues_for(base, 0x8080ULL);
    const auto src = haze::test::allocate_and_h2d_residues(inputs, base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);

    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    for (std::size_t i = 0; i < base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0);
        REQUIRE(hazeMemcpy(got.data(), dst[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            const uint64_t want = haze::test::add_mod(inputs[i][k], inputs[i][k], base[i]);
            if (got[k] != want) {
                INFO("residue " << i << " slot " << k);
                REQUIRE(got[k] == want);
            }
        }
    }

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("recorded inputs: an op-time promotion is not mistaken for an upload", "[integration]") {
    // A memset buffer has no uploaded bytes and no declared prime, yet it is a
    // live-in the moment an op promotes it. Inferring "uploaded modulus-less"
    // from live-in-ness therefore refuses it - and only from the second operand
    // build onward, since the first promotes it. The refusal must key on what
    // the upload recorded, so this stays legal and order-independent.
    const auto base = haze::test::setup_integration_mrp3_config(kRingDim, kQ0);
    const auto declared =
        haze::test::allocate_and_h2d_residues(residues_for(base, 0x9090ULL), base);
    const auto zeroed = haze::test::allocate_dst_residues(base.size(), kBytes);
    for (void *p : zeroed)
        REQUIRE(hazeMemset(p, 0, kBytes) == HAZE_SUCCESS);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);

    for (int pass = 0; pass < 2; ++pass) {
        INFO("pass " << pass);
        REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(declared).data(),
                           haze::test::to_const(zeroed).data(), base.data(), base.size(),
                           nullptr) == HAZE_SUCCESS);
    }
    // Both operands of one op may be the same promoted buffer.
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(zeroed).data(),
                       haze::test::to_const(zeroed).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    haze::test::free_all_residues(declared);
    haze::test::free_all_residues(zeroed);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("spilled inputs are consumed by the post-recording hook", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};

    // Control: an ordinary recording. The tag path auto-spills the upload (seed A)
    // and the hook consumes it, so the .bin carries A.
    std::filesystem::remove_all("haze_spill_control");
    const auto control_dir = record_one_group("haze_spill_control", base);

    // Variant: same upload seed, but every src addr is freed right after the compute
    // that consumed it (a temporary upload's ordinary lifetime), well before the flush.
    // bind_name snapshots a tag's residues onto their own file at tag time, so freeing
    // the addrs afterward cannot perturb what the hook reads: the resulting .bin must
    // still match the control's byte for byte.
    std::filesystem::remove_all("haze_spill_freed_before_flush");
    configure("haze_spill_freed_before_flush", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x5150ULL), base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    haze::test::free_all_residues(src);
    for (void *p : src)
        REQUIRE_FALSE(haze::input_spill().has(haze::to_dev_addr(p)));

    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const std::filesystem::path freed_dir{"haze_spill_freed_before_flush"};
    const auto control_entries = read_manifest(control_dir, "haze_spill_control");
    const auto freed_entries = read_manifest(freed_dir, "haze_spill_freed_before_flush");
    REQUIRE(control_entries.size() == 1);
    REQUIRE(freed_entries.size() == 1);
    REQUIRE(slurp(control_dir / control_entries.front().bin_file) ==
            slurp(freed_dir / freed_entries.front().bin_file));

    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(control_dir);
    std::filesystem::remove_all(freed_dir);
}

TEST_CASE("a missing spill record fails the flush", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_spill_missing";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xC000ULL), base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);

    // Consume the group's own name binding directly (bytes untouched) so the hook's
    // take_named() finds nothing and fails the flush rather than falling back to
    // the trace's own values.
    REQUIRE(haze::input_spill().take_named("haze_mrp_in_0").has_value());
    REQUIRE(hazeWriteProgram() != HAZE_SUCCESS);

    // A second attempt must not crash: finalize_locked already cleared
    // recording_ on the first (failed) flush, and must not silently produce a
    // zero-filled .bin for the group.
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    REQUIRE_FALSE(std::filesystem::exists(dir / (program_name + ".input_haze_mrp_in_0.bin")));

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("pre-flush D2H of a tagged input is served from the spill store", "[integration]") {
    // SRP: a plain modulus-less H2D upload.
    {
        const std::vector<uint64_t> base = {kQ0};
        configure("haze_spill_srp_d2h", base);
        const auto residues = residues_for(base, 0xD100ULL);
        const auto src = haze::test::allocate_and_h2d_residues(residues);
        REQUIRE_FALSE(haze::test::AllocatorTestAccess::with_shadow_data(
            haze::allocator(), haze::to_dev_addr(src[0]), [](const uint64_t *, std::size_t) {}));

        std::vector<uint64_t> full(kRingDim, 0);
        REQUIRE(hazeMemcpy(full.data(), src[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        REQUIRE(full == residues[0]);

        std::vector<uint64_t> half(kRingDim / 2, 0);
        REQUIRE(hazeMemcpy(half.data(), src[0], kBytes / 2, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::size_t i = 0; i < kRingDim / 2; ++i)
            REQUIRE(half[i] == residues[0][i]);

        haze::test::free_all_residues(src);
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    }

    // MRP: a declared-modulus group upload.
    {
        const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
        configure("haze_spill_mrp_d2h", base);
        const auto residues = residues_for(base, 0xD200ULL);
        const auto src = haze::test::allocate_and_h2d_residues(residues, base);
        for (void *p : src) {
            REQUIRE_FALSE(haze::test::AllocatorTestAccess::with_shadow_data(
                haze::allocator(), haze::to_dev_addr(p), [](const uint64_t *, std::size_t) {}));
        }

        for (std::size_t i = 0; i < base.size(); ++i) {
            std::vector<uint64_t> full(kRingDim, 0);
            REQUIRE(hazeMemcpy(full.data(), src[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                    HAZE_SUCCESS);
            REQUIRE(full == residues[i]);

            std::vector<uint64_t> half(kRingDim / 2, 0);
            REQUIRE(hazeMemcpy(half.data(), src[i], kBytes / 2, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                    HAZE_SUCCESS);
            for (std::size_t k = 0; k < kRingDim / 2; ++k)
                REQUIRE(half[k] == residues[i][k]);
        }

        haze::test::free_all_residues(src);
        REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    }
}

TEST_CASE("post-flush D2H of a pure input still returns its uploaded value", "[integration]") {
    // Address lifetime, not epoch lifetime: a flush only clears the per-recording name
    // manifest (clear_names), so a never-freed, never-overwritten input's residue is
    // still readable after the flush that consumed its name binding.
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_spill_post_flush", base);
    const auto residues = residues_for(base, 0xD300ULL);
    const auto src = haze::test::allocate_and_h2d_residues(residues);
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    // src[0] must feed a tagged output, or nothing is pending and hazeWriteProgram
    // is a true no-op (finalize_locked) that never reaches clear_names either.
    REQUIRE(hazeAdd(dst, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), src[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out == residues[0]);

    haze::test::free_all_residues(src);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("cross-epoch reuse of a flushed input records correctly", "[integration]") {
    const std::vector<uint64_t> base = {kQ0};
    const std::string program_name = "haze_spill_cross_epoch";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xD400ULL));

    void *dst1 = nullptr;
    REQUIRE(hazeMalloc(&dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst1, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst1) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries_1 = read_manifest(dir, program_name);
    REQUIRE(entries_1.size() == 1);
    const std::string first_bin = slurp(dir / entries_1.front().bin_file);

    // A new epoch, reusing src[0] without re-uploading: the address's residue is
    // still on disk from the first tag (address lifetime, not epoch lifetime), so
    // lookup_or_create_locked's input_spill().has(addr) path binds a fresh name
    // for this recording without touching the bytes.
    void *dst2 = nullptr;
    REQUIRE(hazeMalloc(&dst2, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst2, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst2) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries_2 = read_manifest(dir, program_name);
    REQUIRE(entries_2.size() == 1);
    // Same program, same input name, same underlying bytes: the second project's
    // input .bin is byte-identical to the first's.
    REQUIRE(slurp(dir / entries_2.front().bin_file) == first_bin);

    haze::test::free_all_residues(src);
    REQUIRE(hazeFree(dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst2) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("partial H2D over a tagged input lands on a fresh zero-tailed shadow", "[integration]") {
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_spill_partial_h2d", base);
    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);
    const auto full_residue = residues_for(base, 0xD500ULL).front();
    REQUIRE(hazeMemcpy(dev, full_residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);

    // Re-upload only the first half over the same addr: the first tag already
    // evicted shadow_data_[dev], so copy_h2d's second call recreates a fresh,
    // zero-filled shadow before writing the half it's given - not the first
    // upload's stale tail.
    const auto half_residue = residues_for(base, 0xD600ULL).front();
    REQUIRE(hazeMemcpy(dev, half_residue.data(), kBytes / 2, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);

    std::vector<uint64_t> expected(kRingDim, 0);
    for (std::size_t i = 0; i < kRingDim / 2; ++i)
        expected[i] = half_residue[i];

    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL); // poison: a short read must be visible
    REQUIRE(hazeMemcpy(got.data(), dev, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(got == expected);

    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("memset over a tagged input drops that residue's spill serving", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    configure("haze_spill_memset_drops_binding", base);
    const auto residues = residues_for(base, 0xD700ULL);
    const auto src = haze::test::allocate_and_h2d_residues(residues, base);

    REQUIRE(hazeMemset(src[0], 0x5A, kBytes) == HAZE_SUCCESS);

    // The memset'd residue's binding was dropped by invalidate(), so its D2H
    // now falls through to the (freshly memset) shadow, not the spill store.
    std::vector<uint8_t> memset_bytes(kBytes, 0);
    REQUIRE(hazeMemcpy(memset_bytes.data(), src[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    for (uint8_t b : memset_bytes)
        REQUIRE(b == 0x5A);

    // The other residues' bindings are untouched: still served from the spill store.
    for (std::size_t i = 1; i < base.size(); ++i) {
        std::vector<uint64_t> got(kRingDim, 0);
        REQUIRE(hazeMemcpy(got.data(), src[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(got == residues[i]);
    }

    haze::test::free_all_residues(src);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("spill activation failure prevents recording, loudly", "[integration]") {
    const std::vector<uint64_t> base = {kQ0};
    const std::string program_name = "haze_spill_activation_failure";
    const std::filesystem::path dir{program_name};
    // The spill root is a process-wide sibling of the program dir (cwd/haze_input_spill),
    // not a child of it; configure()'s hazeDeviceReset() already cleared it.
    const std::filesystem::path spill_root{"haze_input_spill"};
    std::filesystem::remove_all(dir);
    configure(program_name, base);

    // Block the root with a regular file before the first H2D, so
    // ensure_recording_locked's activate() fails and no recording starts.
    std::filesystem::remove_all(spill_root);
    {
        std::ofstream blocker(spill_root);
        blocker << "not a directory";
    }

    void *dev = nullptr;
    REQUIRE(hazeMalloc(&dev, kBytes) == HAZE_SUCCESS);
    const auto residue = residues_for(base, 0xD800ULL).front();
    // The H2D write itself succeeds: it's a plain shadow write, since the tag
    // path no-ops when recording never started.
    REQUIRE(hazeMemcpy(dev, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    // The compute op retries activation (still blocked) and fails loudly instead
    // of silently recording nothing.
    REQUIRE(hazeAdd(dst, dev, dev, 0, nullptr) == HAZE_ERROR_INTERNAL);
    hazeGetLastError();

    REQUIRE(hazeFree(dev) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
    // Restore the shared root so later tests in this binary can activate it.
    std::filesystem::remove_all(spill_root);
}

TEST_CASE("an input never used in a modulus-bearing op is dropped from inputs.json "
          "and its spill file is freed with the address",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_spill_unused_input";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);

    // SRP upload with no compute use: a plain modulus-less H2D never touched by
    // an op, so sync_fhetch_state_to_compiler drops it from inputs.json (no
    // modulus-map entry). Its spill file is address-scoped, not epoch-scoped, so
    // the flush leaves it alone; only freeing the address retires it.
    const auto unused = haze::test::allocate_and_h2d_residues(residues_for({kQ0}, 0xD900ULL));
    void *unused_ptr = unused[0];

    // One MRP group with a compute, so the flush has something to tag and replay.
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xDA00ULL), base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);

    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries = read_manifest(dir, program_name);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries.front().name == "haze_mrp_in_0");
    // Dropped from the manifest, but the flush left its bytes alone.
    REQUIRE(std::filesystem::exists(spill_file_for(unused_ptr)));

    REQUIRE(hazeFree(unused_ptr) == HAZE_SUCCESS);
    REQUIRE_FALSE(std::filesystem::exists(spill_file_for(unused_ptr)));

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("fhetch input registry is released at flush, not held for the next reset",
          "[integration]") {
    // get_input_ring_dimension() reads the first registered input's ring dimension
    // (0 with none registered), so it doubles as an emptiness probe for the vendor
    // input_registry() that reset_for_epoch() releases.
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_fhetch_registry_release";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == 0);

    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xE000ULL), base);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == kRingDim);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    // A successful flush releases the vendor registry immediately, well before
    // any later hazeDeviceReset / Compiler::start() would.
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == 0);

    // The release left nothing behind that a following recording needs.
    const auto src2 = haze::test::allocate_and_h2d_residues(residues_for(base, 0xE100ULL), base);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == kRingDim);
    const auto dst2 = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst2.data(), haze::test::to_const(src2).data(),
                       haze::test::to_const(src2).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst2)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == 0);

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    haze::test::free_all_residues(src2);
    haze::test::free_all_residues(dst2);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("fhetch input registry survives a failed flush; hazeDeviceReset still clears it",
          "[integration]") {
    // Success-only gate: a failed stop can leave the vendor recorder open, so
    // releasing here would roll the address counter back under a live trace. The
    // registries stay populated until the device resets.
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_fhetch_registry_failed_flush";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0xE200ULL), base);
    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);

    // Consume the group's own name binding (test_input_entries.cpp's "a missing
    // spill record fails the flush" construction) so the hook's take_named()
    // finds nothing and fails the flush.
    REQUIRE(haze::input_spill().take_named("haze_mrp_in_0").has_value());
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == kRingDim);
    REQUIRE(hazeWriteProgram() != HAZE_SUCCESS);

    // The failed flush did not release the registry.
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == kRingDim);

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(niobium::fhetch::get_input_ring_dimension() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a failed MRP tag restores every shadow and leaves no spill entries", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const auto residues = residues_for(base, 0xF100ULL);
    const std::filesystem::path spill_root{"haze_input_spill"};

    // Control: the identical upload with no interference, its own program dir. Its
    // input .bin is the byte-for-byte oracle the recovered upload below must match.
    const std::string control_name = "haze_spill_mrp_tag_failure_control";
    const std::filesystem::path control_dir{control_name};
    std::filesystem::remove_all(control_dir);
    configure(control_name, base);
    {
        const auto control_src = haze::test::allocate_and_h2d_residues(residues, base);
        const auto control_dst = haze::test::allocate_dst_residues(base.size(), kBytes);
        REQUIRE(hazeAddMrp(control_dst.data(), haze::test::to_const(control_src).data(),
                           haze::test::to_const(control_src).data(), base.data(), base.size(),
                           nullptr) == HAZE_SUCCESS);
        for (void *out : control_dst)
            REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
        REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
        haze::test::free_all_residues(control_src);
        haze::test::free_all_residues(control_dst);
    }

    const std::string program_name = "haze_spill_mrp_tag_failure";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);

    // A throwaway SRP upload activates the spill store first: activate() itself
    // creates haze_input_spill, so the chmod below must land after that, or it would
    // make activate() (not put()) the failure point.
    void *scratch = nullptr;
    REQUIRE(hazeMalloc(&scratch, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(scratch, residues_for({kQ0}, 0xF000ULL).front().data(), kBytes,
                       HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(std::filesystem::is_directory(spill_root));

    std::vector<void *> ptrs(residues.size(), nullptr);
    std::vector<const void *> srcs(residues.size(), nullptr);
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < residues.size(); ++i) {
        bytes = residues[i].size() * sizeof(uint64_t);
        REQUIRE(hazeMalloc(&ptrs[i], bytes) == HAZE_SUCCESS);
        srcs[i] = residues[i].data();
    }

    // Block writes under the spill root so put() (not the shadow writes copy_h2d_mrp
    // already did) is what fails. No REQUIRE runs between the chmod and its restore,
    // so a failed assertion can never leave the directory unwritable for cleanup.
    const std::filesystem::perms original = std::filesystem::status(spill_root).permissions();
    // error_code overloads throughout: a throw here (instead of a returned error) would
    // skip the restore below and strand the directory at 0500 for later cases.
    std::error_code restrict_ec;
    std::filesystem::permissions(
        spill_root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, restrict_ec);
    const hazeError_t mrp_result = hazeMemcpyMrp(
        ptrs.data(), srcs.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(), base.size());
    std::error_code restore_ec;
    std::filesystem::permissions(spill_root, original, std::filesystem::perm_options::replace,
                                 restore_ec);
    REQUIRE_FALSE(restrict_ec);
    REQUIRE_FALSE(restore_ec);
    REQUIRE(mrp_result != HAZE_SUCCESS);
    hazeGetLastError();

    // Every addr's shadow is restored: none carries a spill record, and a D2H returns
    // exactly the bytes just uploaded.
    for (std::size_t i = 0; i < base.size(); ++i) {
        REQUIRE_FALSE(haze::input_spill().has(haze::to_dev_addr(ptrs[i])));
        std::vector<uint64_t> got(residues[i].size(), 0);
        REQUIRE(hazeMemcpy(got.data(), ptrs[i], bytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(got == residues[i]);
    }

    // A retry of the identical upload succeeds now the directory is writable again.
    REQUIRE(hazeMemcpyMrp(ptrs.data(), srcs.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(),
                          base.size()) == HAZE_SUCCESS);

    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(ptrs).data(),
                       haze::test::to_const(ptrs).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries = read_manifest(dir, program_name);
    const auto control_entries = read_manifest(control_dir, control_name);
    // 1, not 2: `scratch` was never used by any op, so it is dropped as a never-read
    // input (see "an input never used in a modulus-bearing op is dropped from
    // inputs.json"); only the retried group's haze_mrp_in_0 entry survives.
    REQUIRE(entries.size() == 1);
    REQUIRE(control_entries.size() == 1);
    REQUIRE(slurp(dir / entries.front().bin_file) ==
            slurp(control_dir / control_entries.front().bin_file));

    REQUIRE(hazeFree(scratch) == HAZE_SUCCESS);
    haze::test::free_all_residues(ptrs);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(control_dir);
    std::filesystem::remove_all(spill_root);
}

TEST_CASE("a failed MRP tag mid-way through undoes an already-spilled prefix", "[integration]") {
    // The chmod-based failure above blocks the whole spill root, so it fails index 0's
    // put() before anything is on disk. This covers the harder case: index 0 already
    // committed to disk when index 1 fails, so the rollback must UNDO a real prefix, not
    // just refrain from writing one. put()'s scratch path is
    // <spill_root>/addr_<hex>.spill.tmp, and an ofstream can't open an existing directory
    // for writing (see test_input_spill.cpp's mechanism check), so pre-creating one there
    // fails exactly index 1's put() and leaves index 0's and index 2's untouched by the
    // blocker itself.
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const auto residues = residues_for(base, 0xF200ULL);
    const std::filesystem::path spill_root{"haze_input_spill"};

    const std::string program_name = "haze_spill_mrp_prefix_rollback";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);

    // A throwaway SRP upload activates the spill store first, so the block below lands on
    // put(), not on ensure_recording_locked's activate().
    void *scratch = nullptr;
    REQUIRE(hazeMalloc(&scratch, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(scratch, residues_for({kQ0}, 0xF300ULL).front().data(), kBytes,
                       HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(std::filesystem::is_directory(spill_root));

    std::vector<void *> ptrs(residues.size(), nullptr);
    std::vector<const void *> srcs(residues.size(), nullptr);
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < residues.size(); ++i) {
        bytes = residues[i].size() * sizeof(uint64_t);
        REQUIRE(hazeMalloc(&ptrs[i], bytes) == HAZE_SUCCESS);
        srcs[i] = residues[i].data();
    }

    std::ostringstream tmp_name;
    tmp_name << "addr_" << std::hex << haze::to_uintptr(haze::to_dev_addr(ptrs[1])) << ".spill.tmp";
    const std::filesystem::path tmp_path = spill_root / tmp_name.str();
    REQUIRE(std::filesystem::create_directory(tmp_path));

    const hazeError_t mrp_result = hazeMemcpyMrp(
        ptrs.data(), srcs.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(), base.size());
    REQUIRE(mrp_result != HAZE_SUCCESS);
    hazeGetLastError();

    // Index 0's put() succeeded before index 1 failed; the rollback undoes it along with
    // the rest. Every addr comes back with no spill record and its shadow restored to
    // exactly the bytes just uploaded.
    for (std::size_t i = 0; i < base.size(); ++i) {
        REQUIRE_FALSE(haze::input_spill().has(haze::to_dev_addr(ptrs[i])));
        std::vector<uint64_t> got(residues[i].size(), 0);
        REQUIRE(hazeMemcpy(got.data(), ptrs[i], bytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(got == residues[i]);
    }

    // A retry of the identical upload succeeds: write_residue_file's own failure cleanup
    // already removed the blocking directory (verified in test_input_spill.cpp).
    REQUIRE(hazeMemcpyMrp(ptrs.data(), srcs.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(),
                          base.size()) == HAZE_SUCCESS);

    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(ptrs).data(),
                       haze::test::to_const(ptrs).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    // 1, not 2: `scratch` was never used by any op, so it is dropped as a never-read input.
    const auto entries = read_manifest(dir, program_name);
    REQUIRE(entries.size() == 1);

    REQUIRE(hazeFree(scratch) == HAZE_SUCCESS);
    haze::test::free_all_residues(ptrs);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(spill_root);
}

TEST_CASE("hazeInputGroupName answers the minted name for every residue of an MRP upload",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_input_group_name_basic";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1000ULL), base);

    char buf[64];
    for (void *p : src) {
        REQUIRE(hazeInputGroupName(p, buf, sizeof buf) == HAZE_SUCCESS);
        REQUIRE(std::string(buf) == "haze_mrp_in_0");
    }

    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries = read_manifest(dir, program_name);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries.front().name == "haze_mrp_in_0");

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("hazeInputGroupName follows a re-upload to the newer group name", "[integration]") {
    // Mirrors "recorded inputs: re-uploading the same addresses records both uploads":
    // each upload builds fresh fhetch polynomials, so the query must track the newer
    // name, not the leading addr's original one.
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::string program_name = "haze_input_group_name_reupload";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);

    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1100ULL), base);
    char buf[64];
    for (void *p : src) {
        REQUIRE(hazeInputGroupName(p, buf, sizeof buf) == HAZE_SUCCESS);
        REQUIRE(std::string(buf) == "haze_mrp_in_0");
    }
    const auto dst1 = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst1.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    const auto second = residues_for(base, 0x1200ULL);
    std::vector<const void *> hosts(base.size(), nullptr);
    for (std::size_t i = 0; i < base.size(); ++i)
        hosts[i] = second[i].data();
    REQUIRE(hazeMemcpyMrp(src.data(), hosts.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base.data(),
                          base.size()) == HAZE_SUCCESS);
    for (void *p : src) {
        REQUIRE(hazeInputGroupName(p, buf, sizeof buf) == HAZE_SUCCESS);
        REQUIRE(std::string(buf) == "haze_mrp_in_1");
    }

    const auto dst2 = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst2.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst1)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    for (void *out : dst2)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    const auto entries = read_manifest(dir, program_name);
    REQUIRE(count_prefixed(entries, "haze_mrp_in_") == 2);

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst1);
    haze::test::free_all_residues(dst2);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("hazeInputGroupName answers SOURCE_UNAVAILABLE once the address is freed or memset",
          "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1};
    configure("haze_input_group_name_freed", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1300ULL), base);
    char buf[64];
    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_SUCCESS);

    REQUIRE(hazeMemset(src[0], 0, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
    // The other residue's binding is untouched by src[0]'s memset.
    REQUIRE(hazeInputGroupName(src[1], buf, sizeof buf) == HAZE_SUCCESS);

    REQUIRE(hazeFree(src[1]) == HAZE_SUCCESS);
    REQUIRE(hazeInputGroupName(src[1], buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();

    REQUIRE(hazeFree(src[0]) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeInputGroupName answers SOURCE_UNAVAILABLE after a real finalize", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    const std::string program_name = "haze_input_group_name_finalize";
    const std::filesystem::path dir{program_name};
    std::filesystem::remove_all(dir);
    configure(program_name, base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1400ULL), base);
    char buf[64];
    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_SUCCESS);

    const auto dst = haze::test::allocate_dst_residues(base.size(), kBytes);
    REQUIRE(hazeAddMrp(dst.data(), haze::test::to_const(src).data(),
                       haze::test::to_const(src).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    for (void *out : dst)
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    // A real finalize: something was tagged, so clear_state_locked wipes the epoch.
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();

    haze::test::free_all_residues(src);
    haze::test::free_all_residues(dst);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    std::filesystem::remove_all(dir);
}

TEST_CASE("hazeInputGroupName still answers after a true no-op write_program", "[integration]") {
    const std::vector<uint64_t> base = {kQ0, kQ1};
    configure("haze_input_group_name_noop_finalize", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1500ULL), base);
    char buf[64];
    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_SUCCESS);
    REQUIRE(std::string(buf) == "haze_mrp_in_0");

    // Nothing tagged: finalize_locked's true-no-op path leaves recording and
    // bindings intact, so the epoch this name belongs to survives.
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);

    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_SUCCESS);
    REQUIRE(std::string(buf) == "haze_mrp_in_0");

    haze::test::free_all_residues(src);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeInputGroupName drops the name once a plain hazeMemcpy(H2D) re-writes "
          "an MRP-uploaded address",
          "[integration]") {
    // The address is now governed by a fresh, unnamed haze_in_<n> entry, not the earlier
    // MRP group: the query index must stop answering the stale name for it.
    const std::vector<uint64_t> base = {kQ0, kQ1};
    configure("haze_input_group_name_plain_h2d_forgets", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1700ULL), base);
    char buf[64];
    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_SUCCESS);
    REQUIRE(hazeInputGroupName(src[1], buf, sizeof buf) == HAZE_SUCCESS);

    const auto plain = residues_for({kQ0}, 0x1800ULL).front();
    REQUIRE(hazeMemcpy(src[0], plain.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeInputGroupName(src[0], buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
    // The untouched sibling residue still answers the original MRP group name.
    REQUIRE(hazeInputGroupName(src[1], buf, sizeof buf) == HAZE_SUCCESS);
    REQUIRE(std::string(buf) == "haze_mrp_in_0");

    haze::test::free_all_residues(src);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeInputGroupName never answers for an output-only address", "[integration]") {
    // A compute result's destination address was never uploaded, so it never entered
    // the query index in the first place - distinct from forget_input_group, which
    // only fires for an address that WAS an upload.
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_input_group_name_output_only", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1900ULL));
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, src[0], src[0], 0, nullptr) == HAZE_SUCCESS);

    char buf[64];
    REQUIRE(hazeInputGroupName(dst, buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();

    haze::test::free_all_residues(src);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeInputGroupName refuses a buffer too short, leaving it untouched", "[integration]") {
    // "haze_mrp_in_0" is 13 chars; the exact fit is 14 bytes (name + NUL). A
    // size()+1 > out_len check alone would let out_len == 13 through and write
    // one byte past a 13-byte buffer, so this pins the boundary on both sides.
    const std::vector<uint64_t> base = {kQ0};
    configure("haze_input_group_name_short_buffer", base);
    const auto src = haze::test::allocate_and_h2d_residues(residues_for(base, 0x1600ULL), base);

    char buf[16];
    std::memset(buf, 0x7E, sizeof buf); // sentinel: must survive a rejected call untouched

    REQUIRE(hazeInputGroupName(src[0], buf, 3) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    for (char c : buf)
        REQUIRE(c == 0x7E);

    // One byte short of the exact fit: still refused, still untouched.
    REQUIRE(hazeInputGroupName(src[0], buf, 13) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    for (char c : buf)
        REQUIRE(c == 0x7E);

    // The exact fit succeeds and writes exactly name.size() + 1 bytes.
    REQUIRE(hazeInputGroupName(src[0], buf, 14) == HAZE_SUCCESS);
    REQUIRE(std::string(buf) == "haze_mrp_in_0");
    REQUIRE(buf[13] == '\0');
    REQUIRE(buf[14] == 0x7E);
    REQUIRE(buf[15] == 0x7E);

    haze::test::free_all_residues(src);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("hazeInputGroupName rejects NULL/zero arguments", "[integration]") {
    char buf[64];
    void *const ptr = reinterpret_cast<void *>(0x1000);

    REQUIRE(hazeInputGroupName(nullptr, buf, sizeof buf) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeInputGroupName(ptr, nullptr, sizeof buf) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeInputGroupName(ptr, buf, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("hazeInputGroupName answers SOURCE_UNAVAILABLE for an unregistered pointer",
          "[integration]") {
    configure("input_group_name_unregistered", {kQ0});
    // Non-null, well-formed arguments, but the address was never uploaded this epoch: the
    // query index misses, exactly as for a freed or finalized address.
    char buf[64];
    void *const never_uploaded = reinterpret_cast<void *>(0x1000);
    REQUIRE(hazeInputGroupName(never_uploaded, buf, sizeof buf) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
