// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// MRP output-group latest-write-wins tests.
//
// record_mrp_group_locked semantics exercised here:
//   1. Identical re-registration → no-op dedup; exactly one group exported.
//   2. Same dst[0], different shape → group replaced; flushed probe reflects
//      the latest residue count and moduli — whether the tag lands before or
//      after the replacement (pending promotions follow it).
//   3. An addr migrating from group A to group B → A evicted wholesale; only B
//      is exported; A's other members read back as standalone SRP values. A
//      tag placed on A before the migration dies with A's MRP view, leaving
//      only the per-residue exports.
//   4. SRP latest-write: a tag after a later compute op exports the final
//      binding, not the binding at the moment hazeTagOutput was called.
//   5. Free + recycle: freed dst[0] evicts its group; a new op on the recycled
//      address registers a fresh group and exports it correctly.

#include "integration_helpers.hpp"
#include "integration_introspect.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/compiler.h>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Shared setup/oracle helpers (integration_helpers.hpp).
using haze::test::add_mod;

std::vector<uint64_t> setup_mrp3() {
    return haze::test::setup_integration_mrp3_config(kRingDim);
}

// ---------------------------------------------------------------------------
// Count the number of haze_mrp_out_* probe files in serialized_probes/.
// The bridge writes one file per tagged MRP group name. Used to assert
// dedup (expect 1) or eviction (expect 0 or 1) after flush.
// ---------------------------------------------------------------------------

std::size_t count_mrp_out_probe_files() {
    namespace fs = std::filesystem;
    const auto probes_dir = niobium::compiler().get_program_directory() / "serialized_probes";
    if (!fs::exists(probes_dir))
        return 0;
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(probes_dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto stem = entry.path().stem().string();
        if (stem.starts_with("haze_mrp_out_"))
            ++count;
    }
    return count;
}

// hazeDeviceReset wipes serialized_probes/ via hazeReplayBridgeReset, but the
// wipe is a no-op for the FIRST case a binary runs: the program dir isn't
// established until recording starts, so leftovers from a previous invocation
// survive into whichever case Catch2 schedules first. Counting tests call this
// right before hazeFlush (recording active → program dir set; probes are only
// written at flush) so their post-flush counts are exact.
void clear_serialized_probes() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(niobium::compiler().get_program_directory() / "serialized_probes", ec);
}

} // namespace

// ===========================================================================
// TC1: identical in-place re-registration is a dedup no-op.
//
// Same dst[0] → same group name (mrp_signature_name derives from dst[0]).
// Running hazeAddMrp(acc ← acc + x) three times re-registers the same addr
// set and moduli on every call; the second and third are no-ops. Exactly one
// haze_mrp_out_* probe file must exist at flush.
// ===========================================================================

TEST_CASE("MRP group reuse: identical in-place re-registration is a dedup no-op", "[integration]") {
    const auto base = setup_mrp3();
    const std::size_t len = base.size(); // 3

    // Seed data: acc starts as residues from seed 0x1000+i; x from seed 0x2000+i.
    std::vector<std::vector<uint64_t>> acc_data(len);
    std::vector<std::vector<uint64_t>> x_data(len);
    for (std::size_t i = 0; i < len; ++i) {
        acc_data[i] = haze::test::make_residue(base[i], 0x1000ULL + i, kRingDim);
        x_data[i] = haze::test::make_residue(base[i], 0x2000ULL + i, kRingDim);
    }

    // Expected: acc + x + x + x (mod q[i]).
    std::vector<std::vector<uint64_t>> expected(len);
    for (std::size_t i = 0; i < len; ++i) {
        expected[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            uint64_t v = acc_data[i][k];
            v = add_mod(v, x_data[i][k], base[i]);
            v = add_mod(v, x_data[i][k], base[i]);
            v = add_mod(v, x_data[i][k], base[i]);
            expected[i][k] = v;
        }
    }

    auto acc = haze::test::allocate_and_h2d_residues(acc_data);
    auto x = haze::test::allocate_and_h2d_residues(x_data);

    // Three in-place accumulations: acc ← acc + x each time.
    // src1 = acc (aliased), src2 = x.  Identical dst[0] → same group name
    // → second and third calls hit the no-op dedup branch.
    for (int iter = 0; iter < 3; ++iter) {
        REQUIRE(hazeAddMrp(acc.data(), haze::test::to_const(acc).data(),
                           haze::test::to_const(x).data(), base.data(), base.size(),
                           nullptr) == HAZE_SUCCESS);
    }

    // Tag acc[0]; this promotes the MRP group so all residues are exported.
    REQUIRE(hazeTagOutput(acc[0]) == HAZE_SUCCESS);
    clear_serialized_probes();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // --- Value check per residue ---
    for (std::size_t i = 0; i < len; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), acc[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("residue " << i << " (mod " << base[i] << ") slot " << k);
            REQUIRE(got[k] == expected[i][k]);
        }
    }

    // --- Probe dedup: only ONE haze_mrp_out_* group was registered ---
    const std::size_t mrp_files = count_mrp_out_probe_files();
    INFO("haze_mrp_out_* probe files found: " << mrp_files);
    REQUIRE(mrp_files == 1);

    // MRP content check: the single group must carry the 3-fold accumulated values.
    haze::test::check_mrp_against_per_residue(base, expected);

    haze::test::free_all_residues(acc);
    haze::test::free_all_residues(x);
}

// ===========================================================================
// TC2 (core repro): same dst[0] re-registered with fewer residues exports
// the latest shape, NOT the original 3-residue stale group.
//
// This test MUST FAIL when the fix in record_mrp_group_locked is
// reverted: on the broken code the 3-residue group is still registered and
// exported, so check_mrp_against_per_residue with base2 finds no matching
// group.
//
// Sequence:
//   op1: hazeAddMrp([a0,a1,a2] ← x1+x1, base3) — 3-residue group
//   op2: hazeAddMrp([a0,a1]    ← y1+y1, base2) — 2-residue group, same dst[0]
//   tag a0 (promotes group); tag a2 (standalone SRP); flush.
//
// Assertions:
//   (a) MRP readback via check_mrp_against_per_residue(base2, op2_values).
//   (b) D2H of a0/a1 == op2 values.
//   (c) D2H of a2 == op1's residue[2] value (its latest binding).
// ===========================================================================

TEST_CASE("MRP group reuse: same dst[0] re-registered with fewer residues exports the latest shape",
          "[integration]") {
    // Need at least 3 primes: base3 for op1, base2 (first two) for op2.
    const auto base3 = setup_mrp3();
    const std::vector<uint64_t> base2 = {base3[0], base3[1]};

    // op1 inputs: x1[0..2] with seeds 0xAA00+i.
    std::vector<std::vector<uint64_t>> x1_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        x1_data[i] = haze::test::make_residue(base3[i], 0xAA00ULL + i, kRingDim);

    // op2 inputs: y1[0..1] with seeds 0xBB00+i (distinct from op1).
    std::vector<std::vector<uint64_t>> y1_data(2);
    for (std::size_t i = 0; i < 2; ++i)
        y1_data[i] = haze::test::make_residue(base2[i], 0xBB00ULL + i, kRingDim);

    // op1 expected: x1[i] + x1[i] mod base3[i].
    std::vector<std::vector<uint64_t>> exp_op1(3);
    for (std::size_t i = 0; i < 3; ++i) {
        exp_op1[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_op1[i][k] = add_mod(x1_data[i][k], x1_data[i][k], base3[i]);
    }

    // op2 expected: y1[i] + y1[i] mod base2[i].
    std::vector<std::vector<uint64_t>> exp_op2(2);
    for (std::size_t i = 0; i < 2; ++i) {
        exp_op2[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_op2[i][k] = add_mod(y1_data[i][k], y1_data[i][k], base2[i]);
    }

    // Allocate destination slots for op1: a[0..2].
    auto a = haze::test::allocate_dst_residues(3, kBytes);
    auto x1 = haze::test::allocate_and_h2d_residues(x1_data);
    auto y1 = haze::test::allocate_and_h2d_residues(y1_data);

    // op1: write [a0,a1,a2] with 3-residue group.
    REQUIRE(hazeAddMrp(a.data(), haze::test::to_const(x1).data(), haze::test::to_const(x1).data(),
                       base3.data(), base3.size(), nullptr) == HAZE_SUCCESS);

    // op2: write [a0,a1] only using base2; same dst[0] → same group name.
    // This should REPLACE the 3-residue group with a 2-residue group.
    const std::vector<void *> a01 = {a[0], a[1]};
    REQUIRE(hazeAddMrp(a01.data(), haze::test::to_const(y1).data(), haze::test::to_const(y1).data(),
                       base2.data(), base2.size(), nullptr) == HAZE_SUCCESS);

    // Tag a[0] (promotes the 2-residue group) and a[2] (standalone — latest
    // binding is op1's result for that residue).
    REQUIRE(hazeTagOutput(a[0]) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(a[2]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // (a) MRP readback: the exported group must be 2-residue (op2 shape).
    //     On broken code, the 3-residue stale group is exported → FAIL here.
    haze::test::check_mrp_against_per_residue(base2, exp_op2);

    // (b) D2H of a0/a1 == op2 results.
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[" << i << "] (op2) slot " << k);
            REQUIRE(got[k] == exp_op2[i][k]);
        }
    }

    // (c) D2H of a2 == op1's residue[2] (latest write to a[2] was op1).
    {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[2], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[2] (op1 residue[2]) slot " << k);
            REQUIRE(got[k] == exp_op1[2][k]);
        }
    }

    haze::test::free_all_residues(a);
    haze::test::free_all_residues(x1);
    haze::test::free_all_residues(y1);
}

// ===========================================================================
// TC2b: tag BEFORE the replacement — the pending promotion follows it.
//
// hazeTagOutput runs before the second op, while the group is still the
// 3-residue shape. the pending MRP-group set stores names (not membership
// snapshots), so the flush exports the replacement 2-residue shape under the
// same name. The dropped member a2 keeps the per-residue tag it got when the
// 3-residue group was promoted, and exports its latest binding (op1's value)
// as a standalone SRP output. Tags pin addrs, not polynomials.
// ===========================================================================

TEST_CASE("MRP group reuse: a tagged group replaced by a smaller shape exports the replacement",
          "[integration]") {
    const auto base3 = setup_mrp3();
    const std::vector<uint64_t> base2 = {base3[0], base3[1]};

    std::vector<std::vector<uint64_t>> x1_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        x1_data[i] = haze::test::make_residue(base3[i], 0xAB00ULL + i, kRingDim);
    std::vector<std::vector<uint64_t>> y1_data(2);
    for (std::size_t i = 0; i < 2; ++i)
        y1_data[i] = haze::test::make_residue(base2[i], 0xBA00ULL + i, kRingDim);

    std::vector<std::vector<uint64_t>> exp_op1(3);
    for (std::size_t i = 0; i < 3; ++i) {
        exp_op1[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_op1[i][k] = add_mod(x1_data[i][k], x1_data[i][k], base3[i]);
    }
    std::vector<std::vector<uint64_t>> exp_op2(2);
    for (std::size_t i = 0; i < 2; ++i) {
        exp_op2[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_op2[i][k] = add_mod(y1_data[i][k], y1_data[i][k], base2[i]);
    }

    auto a = haze::test::allocate_dst_residues(3, kBytes);
    auto x1 = haze::test::allocate_and_h2d_residues(x1_data);
    auto y1 = haze::test::allocate_and_h2d_residues(y1_data);

    // op1 registers the 3-residue group; TAG IT NOW (before the replacement).
    REQUIRE(hazeAddMrp(a.data(), haze::test::to_const(x1).data(), haze::test::to_const(x1).data(),
                       base3.data(), base3.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(a[0]) == HAZE_SUCCESS);

    // op2 replaces the group (same dst[0], 2 residues) AFTER the tag.
    const std::vector<void *> a01 = {a[0], a[1]};
    REQUIRE(hazeAddMrp(a01.data(), haze::test::to_const(y1).data(), haze::test::to_const(y1).data(),
                       base2.data(), base2.size(), nullptr) == HAZE_SUCCESS);

    clear_serialized_probes();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // (a) The exported group is the replacement: one 2-residue MRP, op2 values.
    const std::size_t mrp_files = count_mrp_out_probe_files();
    INFO("haze_mrp_out_* probe files found: " << mrp_files);
    REQUIRE(mrp_files == 1);
    haze::test::check_mrp_against_per_residue(base2, exp_op2);

    // (b) a0/a1: op2 values.
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[" << i << "] (op2) slot " << k);
            REQUIRE(got[k] == exp_op2[i][k]);
        }
    }

    // (c) a2 kept the per-residue tag from the pre-replacement promotion and
    //     exports its latest binding: op1's residue[2].
    {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[2], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[2] (op1 residue[2], standalone after replacement) slot " << k);
            REQUIRE(got[k] == exp_op1[2][k]);
        }
    }

    haze::test::free_all_residues(a);
    haze::test::free_all_residues(x1);
    haze::test::free_all_residues(y1);
}

// ===========================================================================
// TC3: an addr migrating to a differently-named group evicts the old group.
//
// Group A: [a0, a1, a2] ← op_A (distinct seeds).
// Group B: [b0, b1, a2] ← op_B (dst[0] = b0 ≠ a0, but a2 is shared member).
//   → a2 migrating to B evicts A wholesale.
//
// Tag b0 (promotes B); tag a0 and a1 individually (SRP standalone).
// Assertions:
//   (a) check_mrp_against_per_residue(base3, B's expected) passes.
//   (b) Exactly ONE haze_mrp_out_* file (only B; A evicted, never exported).
//   (c) D2H of a0/a1 == group-A op values (their poly_map_ bindings kept).
//   (d) D2H of a2 == B's op value at index 2.
// ===========================================================================

TEST_CASE("MRP group reuse: addr migrating to a new group evicts the old group wholesale",
          "[integration]") {
    // Pins which group is exported (B) and which is evicted (A).
    // A's members that are NOT in B (a0, a1) keep standalone SRP values.
    const auto base = setup_mrp3();

    // op_A inputs: unique seeds to distinguish from op_B results.
    std::vector<std::vector<uint64_t>> xa_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        xa_data[i] = haze::test::make_residue(base[i], 0xCC00ULL + i, kRingDim);

    // op_B inputs: different seeds so B's residue[2] differs from A's.
    std::vector<std::vector<uint64_t>> xb_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        xb_data[i] = haze::test::make_residue(base[i], 0xDD00ULL + i, kRingDim);

    // Expected values for op_A (xa + xa).
    std::vector<std::vector<uint64_t>> exp_a(3);
    for (std::size_t i = 0; i < 3; ++i) {
        exp_a[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_a[i][k] = add_mod(xa_data[i][k], xa_data[i][k], base[i]);
    }

    // Expected values for op_B (xb + xb).
    std::vector<std::vector<uint64_t>> exp_b(3);
    for (std::size_t i = 0; i < 3; ++i) {
        exp_b[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_b[i][k] = add_mod(xb_data[i][k], xb_data[i][k], base[i]);
    }

    // a[0..2] for group A; b[0..1] for group B (b[0] is new dst[0]).
    auto a = haze::test::allocate_dst_residues(3, kBytes);
    auto b = haze::test::allocate_dst_residues(2, kBytes); // b[0], b[1]
    auto xa = haze::test::allocate_and_h2d_residues(xa_data);
    auto xb = haze::test::allocate_and_h2d_residues(xb_data);

    // op_A: [a0,a1,a2] ← xa + xa; registers group A with dst[0]=a[0].
    REQUIRE(hazeAddMrp(a.data(), haze::test::to_const(xa).data(), haze::test::to_const(xa).data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);

    // op_B: [b0,b1,a2] ← xb + xb; dst[0]=b[0] → new group name (B).
    // a[2] was A's residue[2]; it now belongs to B, which evicts A.
    const std::vector<void *> b_dst = {b[0], b[1], a[2]};
    REQUIRE(hazeAddMrp(b_dst.data(), haze::test::to_const(xb).data(),
                       haze::test::to_const(xb).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    // Tag b[0] → promotes group B (exports b[0], b[1], a[2]).
    // Tag a[0] and a[1] as standalone SRP outputs.
    REQUIRE(hazeTagOutput(b[0]) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(a[0]) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(a[1]) == HAZE_SUCCESS);
    clear_serialized_probes();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // (a) MRP group B exports the correct 3-residue values.
    haze::test::check_mrp_against_per_residue(base, exp_b);

    // (b) Exactly one haze_mrp_out_* probe: group A evicted, only B exported.
    const std::size_t mrp_files = count_mrp_out_probe_files();
    INFO("haze_mrp_out_* probe files found: " << mrp_files);
    REQUIRE(mrp_files == 1);

    // (c) D2H a0/a1: standalone SRP values from op_A (poly_map_ kept).
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[" << i << "] (standalone SRP from op_A) slot " << k);
            REQUIRE(got[k] == exp_a[i][k]);
        }
    }

    // (d) D2H a2 == op_B's value at residue[2]; a2 was overwritten by op_B.
    {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[2], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[2] (op_B residue[2]) slot " << k);
            REQUIRE(got[k] == exp_b[2][k]);
        }
    }

    haze::test::free_all_residues(a);
    haze::test::free_all_residues(b);
    haze::test::free_all_residues(xa);
    haze::test::free_all_residues(xb);
}

// ===========================================================================
// TC3b: tag BEFORE the migration — the evicted group's MRP view is dropped.
//
// hazeTagOutput(a0) promotes group A while A still owns [a0,a1,a2]. A later
// op then claims a2 into a differently-named group B, which evicts A — known
// AND pending. Pinned semantics (latest-write-wins, documented at
// hazeTagOutput in haze.h): the flush succeeds, NO MRP is exported (B was
// never tagged; A's promotion died with its eviction), and the per-residue
// tags created by A's promotion survive, exporting each addr's latest
// binding as a standalone SRP output. fhetch::result(A_name, MRP&) fails
// crisply rather than returning A's stale structure.
// ===========================================================================

TEST_CASE("MRP group reuse: a tagged group evicted by addr migration drops its MRP view",
          "[integration]") {
    const auto base = setup_mrp3();

    std::vector<std::vector<uint64_t>> xa_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        xa_data[i] = haze::test::make_residue(base[i], 0xCE00ULL + i, kRingDim);
    std::vector<std::vector<uint64_t>> xb_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        xb_data[i] = haze::test::make_residue(base[i], 0xDE00ULL + i, kRingDim);

    std::vector<std::vector<uint64_t>> exp_a(3);
    std::vector<std::vector<uint64_t>> exp_b(3);
    for (std::size_t i = 0; i < 3; ++i) {
        exp_a[i].resize(kRingDim);
        exp_b[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            exp_a[i][k] = add_mod(xa_data[i][k], xa_data[i][k], base[i]);
            exp_b[i][k] = add_mod(xb_data[i][k], xb_data[i][k], base[i]);
        }
    }

    auto a = haze::test::allocate_dst_residues(3, kBytes);
    auto b = haze::test::allocate_dst_residues(2, kBytes);
    auto xa = haze::test::allocate_and_h2d_residues(xa_data);
    auto xb = haze::test::allocate_and_h2d_residues(xb_data);

    // op_A registers group A; TAG IT NOW, before the migration.
    REQUIRE(hazeAddMrp(a.data(), haze::test::to_const(xa).data(), haze::test::to_const(xa).data(),
                       base.data(), base.size(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(a[0]) == HAZE_SUCCESS);

    // op_B claims a[2] under a different dst[0] → evicts A (and A's pending
    // promotion). B itself is never tagged.
    const std::vector<void *> b_dst = {b[0], b[1], a[2]};
    REQUIRE(hazeAddMrp(b_dst.data(), haze::test::to_const(xb).data(),
                       haze::test::to_const(xb).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    clear_serialized_probes();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // (a) No MRP export at all: A was evicted after its promotion, B untagged.
    const std::size_t mrp_files = count_mrp_out_probe_files();
    INFO("haze_mrp_out_* probe files found: " << mrp_files);
    REQUIRE(mrp_files == 0);

    // (b) A's per-residue tags survive: a0/a1 export op_A's values...
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[" << i << "] (op_A, standalone SRP) slot " << k);
            REQUIRE(got[k] == exp_a[i][k]);
        }
    }

    // ...and a2 exports its latest binding (op_B's residue[2]).
    {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), a[2], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("a[2] (op_B residue[2]) slot " << k);
            REQUIRE(got[k] == exp_b[2][k]);
        }
    }

    // (c) b0/b1 were never tagged by anyone: D2H must refuse.
    std::vector<uint64_t> scratch(kRingDim, 0);
    REQUIRE(hazeMemcpy(scratch.data(), b[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);

    haze::test::free_all_residues(a);
    haze::test::free_all_residues(b);
    haze::test::free_all_residues(xa);
    haze::test::free_all_residues(xb);
}

// ===========================================================================
// TC4: SRP tag-then-overwrite — the tag exports the final value at flush.
//
// Contract: hazeTagOutput pins the address, not the polynomial bound to the
// address at tag time. The poly_map_ binding at flush time (the latest write)
// is what gets exported. hazeAdd after hazeTagOutput must therefore still
// produce the correct final sum.
//
// Sequence:
//   src1, src2, src3 ← distinct known H2D residues.
//   dst ← src1 + src2     (compute)
//   hazeTagOutput(dst)     (tag while dst = src1+src2)
//   dst ← dst + src3       (second compute, overwrites binding)
//   hazeFlush()
//   D2H dst == src1+src2+src3 mod q  (final value wins)
// ===========================================================================

TEST_CASE("SRP tag-then-overwrite: the tag exports the final value at flush", "[integration]") {
    // Single-residue path; setup_integration_compute_config returns the picked q.
    // Default desired modulus = the suite's standard slot-0 prime.
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);

    const auto v1 = haze::test::make_residue(q, 0xEE01ULL, kRingDim);
    const auto v2 = haze::test::make_residue(q, 0xEE02ULL, kRingDim);
    const auto v3 = haze::test::make_residue(q, 0xEE03ULL, kRingDim);

    // Expected: (v1 + v2 + v3) mod q.
    std::vector<uint64_t> expected(kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k)
        expected[k] = add_mod(add_mod(v1[k], v2[k], q), v3[k], q);

    void *src1 = nullptr;
    void *src2 = nullptr;
    void *src3 = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src2, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src3, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    REQUIRE(hazeMemcpy(src1, v1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src2, v2.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src3, v3.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // First compute: dst = src1 + src2.
    REQUIRE(hazeAdd(dst, src1, src2, 0, nullptr) == HAZE_SUCCESS);

    // Tag while binding is (src1+src2) — contract: tag pins the ADDR not the poly.
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);

    // Second compute AFTER the tag: dst = dst + src3.
    REQUIRE(hazeAdd(dst, dst, src3, 0, nullptr) == HAZE_SUCCESS);

    // Flush: final binding (src1+src2+src3) must be what gets materialized.
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(got.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k);
        REQUIRE(got[k] == expected[k]);
    }

    REQUIRE(hazeFree(src1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src2) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src3) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

// ===========================================================================
// TC5: freed and recycled dst[0] starts clean — eviction on free + fresh op.
//
// hazeFree(a[0]) calls EpochState::invalidate which evicts any MRP group that
// included a[0]. The next hazeMalloc with an equal poly_bytes_ must recycle
// the same DevAddr (LIFO pool). An MRP op on the recycled slot with a new
// shape must register a fresh group and export it.
//
// Sequence:
//   op1: [a0,a1,a2] ← base3 group  (registered group A)
//   hazeFree(a[0])                  (evicts group A; a1/a2 keep poly_map_)
//   hazeMalloc(a0_recycled)         (REQUIRE same address as a[0])
//   H2D fresh data into recycled buffer
//   op2: [a0_recycled, c1] ← base2  (fresh 2-residue group, same device addr)
//   tag + flush + verify via check_mrp_against_per_residue(base2, expected)
// ===========================================================================

TEST_CASE("MRP group reuse: freed and recycled dst[0] starts clean", "[integration]") {
    const auto base3 = setup_mrp3();
    const std::vector<uint64_t> base2 = {base3[0], base3[1]};

    // op1 input data (3 residues).
    std::vector<std::vector<uint64_t>> xa_data(3);
    for (std::size_t i = 0; i < 3; ++i)
        xa_data[i] = haze::test::make_residue(base3[i], 0xFF00ULL + i, kRingDim);

    // op2 input data (2 residues, different seeds).
    std::vector<std::vector<uint64_t>> xb_data(2);
    for (std::size_t i = 0; i < 2; ++i)
        xb_data[i] = haze::test::make_residue(base2[i], 0xFE00ULL + i, kRingDim);

    // Expected for op2: xb + xb mod base2[i].
    std::vector<std::vector<uint64_t>> exp_op2(2);
    for (std::size_t i = 0; i < 2; ++i) {
        exp_op2[i].resize(kRingDim);
        for (uint64_t k = 0; k < kRingDim; ++k)
            exp_op2[i][k] = add_mod(xb_data[i][k], xb_data[i][k], base2[i]);
    }

    // Allocate a[0..2] and run op1 to establish group A.
    auto a = haze::test::allocate_dst_residues(3, kBytes);
    auto xa = haze::test::allocate_and_h2d_residues(xa_data);

    REQUIRE(hazeAddMrp(a.data(), haze::test::to_const(xa).data(), haze::test::to_const(xa).data(),
                       base3.data(), base3.size(), nullptr) == HAZE_SUCCESS);

    // Free a[0]: invalidates group A. The LIFO pool recycles this address next.
    void *const a0_addr = a[0];
    REQUIRE(hazeFree(a[0]) == HAZE_SUCCESS);
    a[0] = nullptr; // prevent double-free at cleanup

    // Recycle: hazeMalloc must return the same DevAddr (the allocator pool is
    // LIFO). This REQUIRE validates the test's premise — the rest of the case
    // only proves anything if the address really was recycled — not the group
    // semantics; if the pool strategy ever changes, rework the setup, don't
    // weaken the group assertions below.
    void *a0_recycled = nullptr;
    REQUIRE(hazeMalloc(&a0_recycled, kBytes) == HAZE_SUCCESS);
    REQUIRE(a0_recycled == a0_addr);

    // Allocate c[1] for op2's second residue slot.
    void *c1 = nullptr;
    REQUIRE(hazeMalloc(&c1, kBytes) == HAZE_SUCCESS);

    // H2D fresh op2 inputs (reuse existing xb_data).
    auto xb = haze::test::allocate_and_h2d_residues(xb_data);

    // op2: [a0_recycled, c1] ← xb + xb; fresh 2-residue group.
    const std::vector<void *> dst2 = {a0_recycled, c1};
    REQUIRE(hazeAddMrp(dst2.data(), haze::test::to_const(xb).data(),
                       haze::test::to_const(xb).data(), base2.data(), base2.size(),
                       nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeTagOutput(a0_recycled) == HAZE_SUCCESS);
    clear_serialized_probes();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // --- Value check ---
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), dst2[i], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (uint64_t k = 0; k < kRingDim; ++k) {
            INFO("op2 residue " << i << " slot " << k);
            REQUIRE(got[k] == exp_op2[i][k]);
        }
    }

    // --- MRP group check: exactly one 2-residue group with op2 values ---
    const std::size_t mrp_files = count_mrp_out_probe_files();
    INFO("haze_mrp_out_* probe files found: " << mrp_files);
    REQUIRE(mrp_files == 1);
    haze::test::check_mrp_against_per_residue(base2, exp_op2);

    // Cleanup (a[0] already freed; a[1] and a[2] still allocated).
    REQUIRE(hazeFree(a0_recycled) == HAZE_SUCCESS);
    REQUIRE(hazeFree(c1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a[1]) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a[2]) == HAZE_SUCCESS);
    haze::test::free_all_residues(xa);
    haze::test::free_all_residues(xb);
}
