// Copyright (C) 2026, All rights reserved by Niobium Microsystems.

#include "bootstrap.hpp"

#include <stdexcept>

namespace haze::test::ops {

Ct bootstrap(const OpCtx &ctx, const BootstrapKeys &bk, const Ct &ct, BootstrapVariant variant) {
    switch (variant) {
    case BootstrapVariant::Standard: {
        // Reduce input to a single tower (q_0) — mirrors OpenFHE's
        // ModReduceInternalInPlace(raised, NSD-1) at ckksrns-fhe.cpp:588.
        Ct depleted = clone_ct(ctx, ct);
        while (depleted.towers() > 1)
            depleted = rescale(ctx, depleted);
        Ct raised = mod_raise(ctx, bk, depleted);
        // OpenFHE's bootstrap does ModReduceInternalInPlace(raised, 1)
        // between ModRaise and CtS (ckksrns-fhe.cpp:689), bringing the
        // ciphertext from L0 Q-towers to lEnc=L0-2*compositeDegree.
        // For our path: full 26 Q-towers → lEnc=24+1 (since after the
        // extra rescale below NSD becomes 1).
        raised = rescale(ctx, raised);
        Ct in_slots = linear_transform(ctx, bk, bk.cts_matrices, raised);
        Ct modded = eval_mod(ctx, bk, in_slots);
        return linear_transform(ctx, bk, bk.stc_matrices, modded);
    }
    case BootstrapVariant::StCFirst: {
        Ct in_coeffs = linear_transform(ctx, bk, bk.stc_matrices, ct);
        Ct raised = mod_raise(ctx, bk, in_coeffs);
        Ct in_slots = linear_transform(ctx, bk, bk.cts_matrices, raised);
        return eval_mod(ctx, bk, in_slots);
    }
    }
    throw std::logic_error("haze::test::ops::bootstrap: unhandled BootstrapVariant");
}

} // namespace haze::test::ops
