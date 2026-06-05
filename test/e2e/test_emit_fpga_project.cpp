// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Tool (not a correctness test): emit a CKKS-add haze project for
// out-of-process replay — e.g. ship the directory to the FPGA host and run
// `nbcc_fhetch_replay --project=<dir> --target=<device>`. Tagged [.] so it
// stays out of the default suites; scripts invoke it by name:
//   haze_tests "emit fpga project for replay"
//
// Driven by environment so it needs no argv (Catch2 owns main):
//   HAZE_EMIT_OUT     (required) absolute output directory for the project.
//   HAZE_EMIT_REPLAY  (optional, any value) run the in-process simulator
//                     (leaves serialized_probes/ as a golden) instead of the
//                     default hazeWriteProgram() write-without-replay.

#include "openfhe.h"
#include "ops.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <vector>

TEST_CASE("emit fpga project for replay", "[emit][.]") {
    const char *out = std::getenv("HAZE_EMIT_OUT");
    if (out == nullptr || out[0] == '\0') {
        SUCCEED("HAZE_EMIT_OUT unset; skipping fpga-project emit tool");
        return;
    }
    const bool do_replay = std::getenv("HAZE_EMIT_REPLAY") != nullptr;

    using namespace lbcrypto;
    namespace ops = haze::test::ops;

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Land the project at exactly HAZE_EMIT_OUT (not cwd/<program_name>); set
    // before the first compute op opens the epoch.
    REQUIRE(hazeSetProgramDirectory(out) == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramInfo("haze_add", "0.1", "haze CKKS add for FPGA replay") == HAZE_SUCCESS);

    ops::OpCtx ctx = ops::make_ctx({.mode = FIXEDMANUAL,
                                    .mult_depth = 1,
                                    .scaling_mod_size = 50,
                                    .batch_size = 8,
                                    .ring_dim = ops::RingDimChoice::OpenFHEDerives()});

    const std::vector<double> a_vals = {1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8};
    const std::vector<double> b_vals = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5};
    const Plaintext pt_a = ctx.cc->MakeCKKSPackedPlaintext(a_vals);
    const Plaintext pt_b = ctx.cc->MakeCKKSPackedPlaintext(b_vals);
    const auto ct_a = ctx.cc->Encrypt(ctx.keys.publicKey, pt_a);
    const auto ct_b = ctx.cc->Encrypt(ctx.keys.publicKey, pt_b);

    const ops::Ct a = ops::h2d_ct(ctx, ct_a);
    const ops::Ct b = ops::h2d_ct(ctx, ct_b);
    const ops::Ct sum = ops::add(ctx, a, b);

    if (do_replay) {
        // In-process simulator: writes serialized_probes/, usable as a golden
        // to diff against the FPGA result for the same project.
        ops::flush_cts({&sum});
        const ops::CtBytes bytes = ops::d2h_ct(ctx, sum);
        REQUIRE(bytes.c0.size() == sum.towers());
        INFO("emitted project + simulator probes to " << out);
    } else {
        // Write-without-replay: hazeWriteProgram is the finalize step here, so
        // there is no flush (replay is the whole thing being skipped). Tag the
        // result so the emitted outputs.json lists it, then write the dir to
        // ship to the FPGA host.
        REQUIRE(sum.towers() > 0);
        ops::tag_ct(sum);
        REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
        INFO("emitted project (no replay) to " << out);
    }
}
