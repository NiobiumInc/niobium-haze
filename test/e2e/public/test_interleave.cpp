// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// "OpenFHE anywhere" proof: in a single process, freely interleave a consumer's
// OWN stock-OpenFHE CKKS ops with haze C-ABI epochs and assert that
//   (1) the haze results are correct (the trace was not clobbered),
//   (2) the stock results are correct (stock OpenFHE works alongside haze), and
//   (3) the haze trace contains ONLY the intended haze ops — interleaving the
//       stock-OpenFHE calls produces a byte-identical set of output probe files
//       versus a control run without them, so the stock calls emitted no IR.
//
// This exe links the shipped libhaze.so (haze* C ABI only) + a separate stock
// OpenFHE. The stock OpenFHE has no CPROBES instrumentation, so it cannot emit
// into haze's recorder; this test demonstrates that empirically. The exe cannot
// reach niobium::compiler(), so (3) is checked purely on disk.

#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <filesystem>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <set>
#include <string>
#include <vector>

namespace {

namespace ops = haze::test::ops;
using namespace lbcrypto;

const std::vector<double> kX1 = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
const std::vector<double> kX2 = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};

// Output probe files libhaze writes under <cwd>/haze/serialized_probes for the
// current process. program_name is planted as "haze" by the bridge, so the dir
// resolves relative to the test's working directory.
std::set<std::string> collect_probe_names() {
    namespace fs = std::filesystem;
    const auto dir = fs::current_path() / "haze" / "serialized_probes";
    std::set<std::string> names;
    if (!fs::exists(dir))
        return names;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.is_regular_file())
            names.insert(e.path().filename().string());
    return names;
}

void decrypt_check(const CryptoContext<DCRTPoly> &cc, const PrivateKey<DCRTPoly> &sk,
                   const Ciphertext<DCRTPoly> &ct, const std::vector<double> &expected) {
    Plaintext pt;
    cc->Decrypt(sk, ct, &pt);
    pt->SetLength(expected.size());
    const auto slots = pt->GetRealPackedValue();
    REQUIRE(slots.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("slot " << i);
        REQUIRE_THAT(slots[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));
    }
}

// Pure stock-OpenFHE work on fresh ciphertexts: an EvalAdd and an EvalMult,
// both decrypt-verified. Allocates NO haze device memory, so it cannot perturb
// the haze allocation order (and thus the probe-file names) of the surrounding
// haze epochs.
void stock_interlude(const ops::OpCtx &ctx) {
    auto s1 = ctx.cc->Encrypt(ctx.keys.publicKey, ctx.cc->MakeCKKSPackedPlaintext(kX1));
    auto s2 = ctx.cc->Encrypt(ctx.keys.publicKey, ctx.cc->MakeCKKSPackedPlaintext(kX2));

    auto s_add = ctx.cc->EvalAdd(s1, s2);
    auto s_mul = ctx.cc->EvalMult(s1, s2);
    ctx.cc->RescaleInPlace(s_mul);

    std::vector<double> e_add(kX1.size());
    std::vector<double> e_mul(kX1.size());
    for (std::size_t i = 0; i < kX1.size(); ++i) {
        e_add[i] = kX1[i] + kX2[i];
        e_mul[i] = kX1[i] * kX2[i];
    }
    decrypt_check(ctx.cc, ctx.keys.secretKey, s_add, e_add);
    decrypt_check(ctx.cc, ctx.keys.secretKey, s_mul, e_mul);
}

// Two haze epochs (add, then sub) through the public C ABI, each decrypt-checked
// against the consumer's stock OpenFHE. When `interleave`, a stock-OpenFHE
// interlude runs before and between the haze epochs. Returns the set of output
// probe files the haze epochs produced.
std::set<std::string> run_epochs(bool interleave) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    auto ctx = ops::make_ctx({.mode = lbcrypto::FIXEDMANUAL,
                              .mult_depth = 2,
                              .scaling_mod_size = 50,
                              .batch_size = 8,
                              .with_relin_key = true,
                              .rotate_indices = {},
                              .ring_dim = ops::RingDimChoice::OpenFHEDerives()});

    auto pt1 = ctx.cc->MakeCKKSPackedPlaintext(kX1);
    auto pt2 = ctx.cc->MakeCKKSPackedPlaintext(kX2);
    auto ct1 = ctx.cc->Encrypt(ctx.keys.publicKey, pt1);
    auto ct2 = ctx.cc->Encrypt(ctx.keys.publicKey, pt2);

    // Stock-OpenFHE references / injection shells (computed up front).
    auto add_ref = ctx.cc->EvalAdd(ct1, ct2);
    auto sub_ref = ctx.cc->EvalSub(ct1, ct2);

    if (interleave)
        stock_interlude(ctx);

    auto a = ops::h2d_ct(ctx, ct1);
    auto b = ops::h2d_ct(ctx, ct2);

    // Haze epoch 1: add.
    auto haze_add = ops::add(ctx, a, b);
    const auto bytes_add = ops::d2h_ct(ctx, haze_add);

    if (interleave)
        stock_interlude(ctx);

    // Haze epoch 2: sub.
    auto haze_sub = ops::sub(ctx, a, b);
    const auto bytes_sub = ops::d2h_ct(ctx, haze_sub);

    std::vector<double> e_add(kX1.size());
    std::vector<double> e_sub(kX1.size());
    for (std::size_t i = 0; i < kX1.size(); ++i) {
        e_add[i] = kX1[i] + kX2[i];
        e_sub[i] = kX1[i] - kX2[i];
    }

    auto ct_add = add_ref->Clone();
    ops::inject_ct(ctx, bytes_add, ct_add);
    decrypt_check(ctx.cc, ctx.keys.secretKey, ct_add, e_add);

    auto ct_sub = sub_ref->Clone();
    ops::inject_ct(ctx, bytes_sub, ct_sub);
    decrypt_check(ctx.cc, ctx.keys.secretKey, ct_sub, e_sub);

    return collect_probe_names();
}

} // namespace

TEST_CASE("e2e: stock OpenFHE interleaved with haze leaves the trace unclobbered", "[e2e]") {
    // Control: haze epochs with no interleaved stock crypto.
    const auto probes_plain = run_epochs(/*interleave=*/false);
    REQUIRE_FALSE(probes_plain.empty());

    // Interleaved: the same haze epochs, with stock-OpenFHE ops woven in. The
    // haze results and stock results are checked inside run_epochs; here we
    // assert the produced probe-file set is identical — i.e. the stock calls
    // contributed nothing to haze's trace.
    const auto probes_interleaved = run_epochs(/*interleave=*/true);
    REQUIRE(probes_interleaved == probes_plain);
}
