// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// End-to-end exercise of the typed kernel layer over the SHIPPED C ABI
// (this exe links libhaze.so + stock OpenFHE only): encrypt with
// OpenFHE, move the ciphertext through haze::cxx kernels (Out<> buffers
// auto-tag — no hazeTagOutput in sight), flush once, inject the
// materialized limbs back into an OpenFHE shell, decrypt, and compare
// against the homomorphic reference. The compute mirrors the additive
// half of test_simple_program.cpp through kernels instead of raw C ABI
// calls.

#include "../ops.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/cxx/haze.hpp>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <openfhe.h>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace ops = haze::test::ops;
using haze::cxx::In;
using haze::cxx::Mrp;
using haze::cxx::Out;
using haze::cxx::Status;

// Ciphertext addition: (a0,a1) + (b0,b1) -> (c0,c1), one kernel.
constexpr auto ct_add = haze::cxx::kernel(
    "ct_add",
    [](In<Mrp> a0, In<Mrp> a1, In<Mrp> b0, In<Mrp> b1, Out<Mrp> c0, Out<Mrp> c1) -> Status {
        if (Status s = haze::cxx::add(*c0, *a0, *b0); !s)
            return s;
        return haze::cxx::add(*c1, *a1, *b1);
    });

// Ciphertext doubling: x + x, exercising operand reuse within a kernel.
constexpr auto ct_double =
    haze::cxx::kernel("ct_double", [](In<Mrp> x0, In<Mrp> x1, Out<Mrp> y0, Out<Mrp> y1) -> Status {
        if (Status s = haze::cxx::add(*y0, *x0, *x0); !s)
            return s;
        return haze::cxx::add(*y1, *x1, *x1);
    });

// Non-owning Mrp view over an ops::Allocs chain: adopt() shares the
// raw allocations with the Ct, release() hands them back before the Ct
// frees them. Declare AFTER the owning Ct so destruction order holds.
class BorrowedMrp {
  public:
    BorrowedMrp(const haze::test::ops::Allocs &chain, std::span<const uint64_t> base)
        : mrp_(Mrp::adopt(chain.context(), {chain.data(), chain.size()}, base)) {}
    ~BorrowedMrp() { (void)mrp_.release(); }
    BorrowedMrp(const BorrowedMrp &) = delete;
    BorrowedMrp &operator=(const BorrowedMrp &) = delete;

    const Mrp &operator*() const noexcept { return mrp_; }

  private:
    Mrp mrp_;
};

ops::CtBytes download_ct(const Mrp &c0, const Mrp &c1, std::size_t ring_dim) {
    ops::CtBytes bytes;
    bytes.c0.resize(c0.base_len(), std::vector<uint64_t>(ring_dim));
    bytes.c1.resize(c1.base_len(), std::vector<uint64_t>(ring_dim));
    for (std::size_t t = 0; t < c0.base_len(); ++t) {
        REQUIRE(c0.download_residue(t, bytes.c0[t]).ok());
        REQUIRE(c1.download_residue(t, bytes.c1[t]).ok());
    }
    return bytes;
}

} // namespace

TEST_CASE("typed kernels: encrypt -> kernel add/double -> decrypt round-trip", "[e2e][cxx]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const ops::OpCtx ctx = ops::make_ctx({
        .mode = lbcrypto::FIXEDMANUAL,
        .mult_depth = 1,
        .scaling_mod_size = 59,
        .batch_size = 8,
        .ring_dim = ops::RingDimChoice::OpenFHEDerives(),
    });
    const auto &cc = ctx.cc;

    const std::vector<double> v1{0.25, 1.5, -2.0, 4.0, 0.0, 3.25, -1.25, 0.5};
    const std::vector<double> v2{1.0, -0.5, 2.5, -3.0, 1.75, 0.25, 2.0, -4.5};
    const auto pt1 = cc->MakeCKKSPackedPlaintext(v1);
    const auto pt2 = cc->MakeCKKSPackedPlaintext(v2);
    auto ct1 = cc->Encrypt(ctx.keys.publicKey, pt1);
    auto ct2 = cc->Encrypt(ctx.keys.publicKey, pt2);

    // Host -> device through the shared ops harness, then borrow the
    // chains as typed handles (the Ct keeps ownership).
    const std::span<const uint64_t> base{ctx.q_base};
    const ops::Ct hct1 = ops::h2d_ct(ctx, ct1);
    const ops::Ct hct2 = ops::h2d_ct(ctx, ct2);
    REQUIRE(hct1.towers() == base.size());
    const BorrowedMrp a0(hct1.c0(), base);
    const BorrowedMrp a1(hct1.c1(), base);
    const BorrowedMrp b0(hct2.c0(), base);
    const BorrowedMrp b1(hct2.c1(), base);

    const std::size_t poly_bytes = ctx.ring_dim * sizeof(uint64_t);
    auto fresh = [&] {
        auto mrp = Mrp::allocate(ctx.haze, base, poly_bytes);
        REQUIRE(mrp.ok());
        return std::move(mrp).value();
    };
    Mrp sum0 = fresh();
    Mrp sum1 = fresh();
    Mrp dbl0 = fresh();
    Mrp dbl1 = fresh();

    // Kernels record; Out<> buffers are tagged automatically.
    REQUIRE(ct_add(*a0, *a1, *b0, *b1, sum0, sum1).ok());
    REQUIRE(ct_double(sum0, sum1, dbl0, dbl1).ok());
    REQUIRE(haze::cxx::flush(ctx.haze).ok());

    // Decrypt both kernel results through OpenFHE shells and compare
    // against the homomorphic reference.
    const auto ref_sum = cc->EvalAdd(ct1, ct2);
    const auto ref_dbl = cc->EvalAdd(ref_sum, ref_sum);

    auto check = [&](const Mrp &c0, const Mrp &c1, const auto &shell_src,
                     const std::vector<double> &expected) {
        auto shell = shell_src->Clone();
        ops::inject_ct(ctx, download_ct(c0, c1, ctx.ring_dim), shell);
        lbcrypto::Plaintext decoded;
        cc->Decrypt(ctx.keys.secretKey, shell, &decoded);
        decoded->SetLength(expected.size());
        const auto &slots = decoded->GetRealPackedValue();
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(slots[i] == Catch::Approx(expected[i]).margin(1e-4));
        }
    };

    std::vector<double> expect_sum(v1.size());
    std::vector<double> expect_dbl(v1.size());
    for (std::size_t i = 0; i < v1.size(); ++i) {
        expect_sum[i] = v1[i] + v2[i];
        expect_dbl[i] = 2.0 * expect_sum[i];
    }
    check(sum0, sum1, ref_sum, expect_sum);
    check(dbl0, dbl1, ref_dbl, expect_dbl);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
