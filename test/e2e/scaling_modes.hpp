// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Per-CKKS-scaling-technique policy structs for E2E tests; iterated by
// Catch2's TEMPLATE_TEST_CASE. Each exposes `kTech` (OpenFHE enum) and
// `kName` (for INFO).

#pragma once

#include "openfhe.h"

namespace haze::test::scaling {

struct FixedManual {
    static constexpr auto kTech = lbcrypto::FIXEDMANUAL;
    static constexpr char const *kName = "FIXEDMANUAL";
};

struct FixedAuto {
    static constexpr auto kTech = lbcrypto::FIXEDAUTO;
    static constexpr char const *kName = "FIXEDAUTO";
};

struct FlexibleAuto {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTO;
    static constexpr char const *kName = "FLEXIBLEAUTO";
};

struct FlexibleAutoExt {
    static constexpr auto kTech = lbcrypto::FLEXIBLEAUTOEXT;
    static constexpr char const *kName = "FLEXIBLEAUTOEXT";
};

} // namespace haze::test::scaling
