// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// One atomic CKKS operation per test case, each pinned at ring dimension
// 65536 — the only dimension the FOG firmware's CC-reorder path accepts, so
// this is the shape that reaches the V80. Every case records exactly one
// operation, so the recorded program IS the operation: the replay figures
// (`firmware FPGA time`, `execute=`) and the emitted device .seq both describe
// that op alone.
//
// Each case still verifies against the OpenFHE oracle — bit-exact limbs, then
// decrypt — because a timing number from an unverified computation is worse
// than no number at all.
//
// Filter: `haze_tests "[atomic-timing]"`, or one op at a time by name.

#include "integration_helpers.hpp"
#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ops = haze::test::ops;

// FOG firmware CC-reorder is 2^16-only; anything else fails with
// "CC reordering only supports ring_dim=65536".
constexpr std::uint32_t kRing = 65536;
constexpr std::uint32_t kMultDepth = 2;
constexpr std::uint32_t kScaleBits = 50;
constexpr std::uint32_t kBatch = 8;

// FLEXIBLEAUTO is what NID runs, so it is the mode the comparison table holds
// across all three targets. Two ops cannot use it: a bare rescale is a no-op
// outside FIXEDMANUAL, and the degree-2 tensor probe needs a mode that does not
// rescale underneath it.
constexpr auto kMode = lbcrypto::FLEXIBLEAUTO;
constexpr char const *kModeName = "FLEXIBLEAUTO";

const std::vector<double> kA = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
const std::vector<double> kB = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};

// Wall time of the flush region. haze records without executing, so this spans
// record -> dispatch -> lower -> replay -> readback: it is the host-inclusive
// figure, NOT device time. Device time comes from the replay's own
// `firmware FPGA time` / `execute=` lines in the same log.
template <class F> double flush_us(F &&f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void report(const char *op, const char *mode, std::size_t towers, double us) {
    std::cout << "[ATOMIC_TIMING] op=" << op << " mode=" << mode << " ring=" << kRing
              << " towers=" << towers << " flush_wall_us=" << std::fixed << std::setprecision(1)
              << us << std::endl;
}

ops::OpCtx make_pinned_ctx(lbcrypto::ScalingTechnique mode, bool with_relin_key,
                           std::vector<std::int32_t> rotate_indices) {
    auto ctx = ops::make_ctx({.mode = mode,
                              .mult_depth = kMultDepth,
                              .scaling_mod_size = kScaleBits,
                              .batch_size = kBatch,
                              .with_relin_key = with_relin_key,
                              .rotate_indices = std::move(rotate_indices),
                              .ring_dim = ops::RingDimChoice::Pinned(kRing)});
    REQUIRE(ctx.ring_dim == kRing);
    REQUIRE(ctx.cc->GetRingDimension() == kRing);
    return ctx;
}

// Bit-exact limb compare on a degree-1 result, then decrypt parity.
void verify_degree1(const ops::OpCtx &ctx, const ops::Ct &result,
                    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct_ref,
                    const ops::CtBytes &haze_bytes) {
    using namespace lbcrypto;

    REQUIRE(result.towers() == ct_ref->GetElements()[0].GetNumOfElements());
    REQUIRE(result.openfhe_level(ctx.q_base.size()) == ct_ref->GetLevel());

    auto ct_haze = ct_ref->Clone();
    ops::inject_ct(ctx, haze_bytes, ct_haze);
    for (std::size_t e = 0; e < 2; ++e) {
        for (std::size_t t = 0; t < result.towers(); ++t) {
            INFO("element " << e << " tower " << t);
            const auto &haze_np =
                ct_haze->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
            const auto &ref_np = ct_ref->GetElements()[e].GetElementAtIndex(static_cast<usint>(t));
            REQUIRE(haze_np.GetValues() == ref_np.GetValues());
        }
    }

    Plaintext pt_haze;
    Plaintext pt_ref;
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_haze, &pt_haze);
    ctx.cc->Decrypt(ctx.keys.secretKey, ct_ref, &pt_ref);
    pt_haze->SetLength(kA.size());
    pt_ref->SetLength(kA.size());
    const auto slots_haze = pt_haze->GetRealPackedValue();
    const auto slots_ref = pt_ref->GetRealPackedValue();
    for (std::size_t i = 0; i < kA.size(); ++i) {
        INFO("slot " << i);
        REQUIRE_THAT(slots_haze[i], Catch::Matchers::WithinAbs(slots_ref[i], 1e-9));
    }
}

} // namespace

TEST_CASE("atomic timing add ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, false, {});

    Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(kA);
    Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(kB);
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    // Oracle before haze compute: CPROBES must not see it.
    auto ct_ref = ctx.cc->EvalAdd(ct_a, ct_b);
    REQUIRE(ct_ref);

    auto a = ops::h2d_ct(ctx, ct_a);
    auto b = ops::h2d_ct(ctx, ct_b);
    auto out = ops::add(ctx, a, b);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("add", kModeName, out.towers(), us);
}

TEST_CASE("atomic timing sub ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, false, {});

    Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(kA);
    Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(kB);
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    auto ct_ref = ctx.cc->EvalSub(ct_a, ct_b);
    REQUIRE(ct_ref);

    auto a = ops::h2d_ct(ctx, ct_a);
    auto b = ops::h2d_ct(ctx, ct_b);
    auto out = ops::sub(ctx, a, b);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("sub", kModeName, out.towers(), us);
}

TEST_CASE("atomic timing scalar-mult ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, false, {});

    const double k = 4.0;
    const std::vector<double> k_vals(kA.size(), k);
    Plaintext pt_x = ctx.cc->MakeCKKSPackedPlaintext(kA);
    Plaintext pt_k = ctx.cc->MakeCKKSPackedPlaintext(k_vals);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x);

    auto ct_ref = ctx.cc->EvalMult(ct, pt_k);
    REQUIRE(ct_ref);

    auto a = ops::h2d_ct(ctx, ct);
    auto out = ops::mult_scalar(ctx, a, pt_k);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("scalar-mult", kModeName, out.towers(), us);
}

TEST_CASE("atomic timing mult-relin ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, /*with_relin_key=*/true, {});

    Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(kA);
    Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(kB);
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    auto ct_ref = ctx.cc->EvalMult(ct_a, ct_b);
    REQUIRE(ct_ref);
    REQUIRE(ct_ref->GetElements().size() == 2);

    auto a = ops::h2d_ct(ctx, ct_a);
    auto b = ops::h2d_ct(ctx, ct_b);
    auto out = ops::mult(ctx, a, b);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("mult-relin", kModeName, out.towers(), us);
}

TEST_CASE("atomic timing rotate ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, false, {1});

    Plaintext pt_x = ctx.cc->MakeCKKSPackedPlaintext(kA);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x);

    auto ct_ref = ctx.cc->EvalRotate(ct, 1);
    REQUIRE(ct_ref);

    auto a = ops::h2d_ct(ctx, ct);
    auto out = ops::rotate(ctx, a, 1);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("rotate", kModeName, out.towers(), us);
}

// FIXEDMANUAL: RescaleInPlace is a no-op in the auto modes, so a bare rescale
// can only be timed here. Reported as such in the table.
TEST_CASE("atomic timing rescale ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(lbcrypto::FIXEDMANUAL, false, {});

    Plaintext pt_x = ctx.cc->MakeCKKSPackedPlaintext(kA);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt_x);

    auto ct_ref = ct->Clone();
    ctx.cc->RescaleInPlace(ct_ref);
    REQUIRE(ct_ref->GetElements()[0].GetNumOfElements() ==
            ct->GetElements()[0].GetNumOfElements() - 1);

    auto a = ops::h2d_ct(ctx, ct);
    auto out = ops::rescale(ctx, a);
    const double us = flush_us([&] { ops::flush_cts({&out}); });
    const auto bytes = ops::d2h_ct(ctx, out);

    verify_degree1(ctx, out, ct_ref, bytes);
    report("rescale", "FIXEDMANUAL", out.towers(), us);
}

// Degree-2 tensor product, no relin: four MRP multiplies and one add, straight
// on the public C ABI. FIXEDMANUAL so nothing rescales underneath the probe.
// Verified limb-exact against cc->GetScheme()->EvalMult, which is the same
// oracle test_openfhe_mul_no_relin_e2e.cpp uses.
TEST_CASE("atomic timing mult-no-relin ring65536", "[integration][e2e][atomic-timing]") {
    using namespace lbcrypto;
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(lbcrypto::FIXEDMANUAL, false, {});

    const auto &base = ctx.q_base;
    REQUIRE(base.size() >= 2);
    const std::size_t towers = base.size();
    const std::size_t bytes_per_residue = ctx.poly_bytes;

    Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(kA);
    Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(kB);
    auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    ConstCiphertext<DCRTPoly> c_a = ct_a;
    ConstCiphertext<DCRTPoly> c_b = ct_b;
    auto ct_ref = ctx.cc->GetScheme()->EvalMult(c_a, c_b);
    REQUIRE(ct_ref);
    REQUIRE(ct_ref->GetElements().size() == 3);
    REQUIRE(ct_ref->GetElements()[0].GetNumOfElements() == towers);

    auto extract = [&](const Ciphertext<DCRTPoly> &c, std::size_t elem) {
        std::vector<std::vector<uint64_t>> chain(towers);
        const auto &e = c->GetElements()[elem];
        for (std::size_t t = 0; t < towers; ++t) {
            const auto &vals = e.GetElementAtIndex(static_cast<usint>(t)).GetValues();
            REQUIRE(vals.GetLength() == kRing);
            chain[t].resize(kRing);
            for (std::size_t i = 0; i < kRing; ++i) {
                chain[t][i] = vals[i].template ConvertToInt<uint64_t>();
            }
        }
        return chain;
    };

    auto da_c0 = haze::test::allocate_and_h2d_residues(extract(ct_a, 0));
    auto da_c1 = haze::test::allocate_and_h2d_residues(extract(ct_a, 1));
    auto db_c0 = haze::test::allocate_and_h2d_residues(extract(ct_b, 0));
    auto db_c1 = haze::test::allocate_and_h2d_residues(extract(ct_b, 1));

    auto d0 = haze::test::allocate_dst_residues(towers, bytes_per_residue);
    auto d1 = haze::test::allocate_dst_residues(towers, bytes_per_residue);
    auto d2 = haze::test::allocate_dst_residues(towers, bytes_per_residue);
    auto t_buf = haze::test::allocate_dst_residues(towers, bytes_per_residue);
    auto u_buf = haze::test::allocate_dst_residues(towers, bytes_per_residue);

    {
        const auto a0 = haze::test::to_const(da_c0);
        const auto a1 = haze::test::to_const(da_c1);
        const auto b0 = haze::test::to_const(db_c0);
        const auto b1 = haze::test::to_const(db_c1);
        REQUIRE(hazeMulMrp(d0.data(), a0.data(), b0.data(), base.data(), towers, nullptr) ==
                HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(t_buf.data(), a0.data(), b1.data(), base.data(), towers, nullptr) ==
                HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(u_buf.data(), a1.data(), b0.data(), base.data(), towers, nullptr) ==
                HAZE_SUCCESS);
        const auto t_c = haze::test::to_const(t_buf);
        const auto u_c = haze::test::to_const(u_buf);
        REQUIRE(hazeAddMrp(d1.data(), t_c.data(), u_c.data(), base.data(), towers, nullptr) ==
                HAZE_SUCCESS);
        REQUIRE(hazeMulMrp(d2.data(), a1.data(), b1.data(), base.data(), towers, nullptr) ==
                HAZE_SUCCESS);
    }

    for (const std::vector<void *> *chain : {&d0, &d1, &d2}) {
        for (void *p : *chain) {
            REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
        }
    }
    const double us = flush_us([&] { REQUIRE(hazeFlush() == HAZE_SUCCESS); });

    auto d2h = [&](const std::vector<void *> &chain) {
        std::vector<std::vector<uint64_t>> host(towers, std::vector<uint64_t>(kRing));
        for (std::size_t t = 0; t < towers; ++t) {
            REQUIRE(hazeMemcpy(host[t].data(), chain[t], bytes_per_residue,
                               HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        }
        return host;
    };
    const auto haze_d0 = d2h(d0);
    const auto haze_d1 = d2h(d1);
    const auto haze_d2 = d2h(d2);

    haze::test::free_all_residues(da_c0);
    haze::test::free_all_residues(da_c1);
    haze::test::free_all_residues(db_c0);
    haze::test::free_all_residues(db_c1);
    haze::test::free_all_residues(t_buf);
    haze::test::free_all_residues(u_buf);
    haze::test::free_all_residues(d0);
    haze::test::free_all_residues(d1);
    haze::test::free_all_residues(d2);

    const auto ref_d0 = extract(ct_ref, 0);
    const auto ref_d1 = extract(ct_ref, 1);
    const auto ref_d2 = extract(ct_ref, 2);
    for (std::size_t t = 0; t < towers; ++t) {
        INFO("tower " << t << " modulus " << base[t]);
        REQUIRE(haze_d0[t] == ref_d0[t]);
        REQUIRE(haze_d1[t] == ref_d1[t]);
        REQUIRE(haze_d2[t] == ref_d2[t]);
    }

    report("mult-no-relin", "FIXEDMANUAL", towers, us);
}

// ---------------------------------------------------------------------------
// Regime 1: N back-to-back ops over a rotating pool of P accumulators.
//
// Every result is consumed by the next use of its accumulator, so nothing is a
// dead store the compiler could eliminate, and the live set is 3P buffers —
// trivially inside the 64-register file, so nothing spills. That is what makes
// this shape expressible through haze at all: the compiler owns register
// allocation, and the only way to keep operands resident is to keep the live
// set small while leaving every value used.
//
// Consecutive ops are independent; the dependency distance is P. Sweeping P
// therefore measures how far apart the dependent uses must be to hide the
// pipeline.
//
// Ping-pong (A -> B -> A) rather than in-place accumulation: dst/src aliasing
// is not documented in the MRP ABI, so it is avoided rather than assumed.
//
// Configured by environment so one binary covers op x N x P:
//   REGIME1_OP   add | mul | ntt | morph   (default add)
//   REGIME1_N    total ops                 (default 1000)
//   REGIME1_POOL accumulators              (default 4)
TEST_CASE("regime1 accumulator sweep ring65536", "[integration][e2e][regime1]") {
    using namespace lbcrypto;

    auto env_or = [](const char *name, const char *fallback) {
        const char *v = std::getenv(name);
        return std::string(v && v[0] ? v : fallback);
    };
    const std::string op = env_or("REGIME1_OP", "add");
    const std::size_t total = static_cast<std::size_t>(std::stoul(env_or("REGIME1_N", "1000")));
    const std::size_t pool = static_cast<std::size_t>(std::stoul(env_or("REGIME1_POOL", "4")));
    REQUIRE(total >= pool);
    REQUIRE(pool >= 1);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    auto ctx = make_pinned_ctx(kMode, false, {});

    // One residue per buffer: a single-modulus MRP op is one instruction's worth
    // of work, which is the unit the ISA-level harness counts.
    const std::vector<std::uint64_t> base = {ctx.q_base.front()};
    const std::size_t bytes = ctx.poly_bytes;

    // Deterministic operands, mod q. The GPU side must generate these with the
    // same formula: the two machines' outputs are compared to each other, so a
    // mismatch in inputs shows up as a mismatch in results.
    auto make_poly = [&](std::size_t idx) {
        std::vector<std::uint64_t> v(kRing);
        std::uint64_t x = 0x9E3779B97F4A7C15ULL * (idx + 1);
        for (std::size_t i = 0; i < kRing; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            v[i] = x % base[0];
        }
        return std::vector<std::vector<std::uint64_t>>{v};
    };

    std::vector<std::vector<void *>> in, acc_a, acc_b;
    for (std::size_t p = 0; p < pool; ++p) {
        in.push_back(haze::test::allocate_and_h2d_residues(make_poly(p)));
        acc_a.push_back(haze::test::allocate_and_h2d_residues(make_poly(pool + p)));
        acc_b.push_back(haze::test::allocate_dst_residues(1, bytes));
    }

    INFO("op=" << op << " N=" << total << " pool=" << pool);

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t n = 0; n < total; ++n) {
        const std::size_t i = n % pool;
        const bool even_round = ((n / pool) % 2) == 0;
        auto &src = even_round ? acc_a[i] : acc_b[i];
        auto &dst = even_round ? acc_b[i] : acc_a[i];
        const auto src_c = haze::test::to_const(src);
        const auto in_c = haze::test::to_const(in[i]);
        if (op == "add") {
            REQUIRE(hazeAddMrp(dst.data(), src_c.data(), in_c.data(), base.data(), base.size(),
                               nullptr) == HAZE_SUCCESS);
        } else if (op == "mul") {
            REQUIRE(hazeMulMrp(dst.data(), src_c.data(), in_c.data(), base.data(), base.size(),
                               nullptr) == HAZE_SUCCESS);
        } else if (op == "ntt") {
            REQUIRE(hazeNTTMrp(dst.data(), src_c.data(), base.data(), base.size(), nullptr) ==
                    HAZE_SUCCESS);
        } else if (op == "morph") {
            REQUIRE(hazeAutomorphMrp(dst.data(), src_c.data(), 5, base.data(), base.size(),
                                     nullptr) == HAZE_SUCCESS);
        } else {
            FAIL("unknown REGIME1_OP: " << op);
        }
    }
    const double record_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();

    // The live results after the last round are the outputs.
    const bool last_round_even = (((total - 1) / pool) % 2) == 0;
    auto &final_acc = last_round_even ? acc_b : acc_a;
    for (std::size_t p = 0; p < pool; ++p) {
        REQUIRE(hazeTagOutput(final_acc[p][0]) == HAZE_SUCCESS);
    }

    const double flush_wall = flush_us([&] { REQUIRE(hazeFlush() == HAZE_SUCCESS); });

    // Checksum every output so the GPU run can be compared without shipping
    // 512 KiB per accumulator around.
    std::uint64_t checksum = 0;
    for (std::size_t p = 0; p < pool; ++p) {
        std::vector<std::uint64_t> host(kRing);
        REQUIRE(hazeMemcpy(host.data(), final_acc[p][0], bytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::uint64_t v : host) {
            checksum = checksum * 1000003ULL + v;
        }
    }

    for (std::size_t p = 0; p < pool; ++p) {
        haze::test::free_all_residues(in[p]);
        haze::test::free_all_residues(acc_a[p]);
        haze::test::free_all_residues(acc_b[p]);
    }

    // The modulus is emitted so the GPU side can build its Limb context on the
    // SAME prime — the two checksums are only comparable if the arithmetic is.
    std::cout << "[REGIME1] backend=haze op=" << op << " n=" << total << " pool=" << pool
              << " ring=" << kRing << " modulus=" << base[0]
              << " record_us=" << std::fixed << std::setprecision(1)
              << record_us << " flush_us=" << flush_wall << " checksum=0x" << std::hex << checksum
              << std::dec << std::endl;
}
