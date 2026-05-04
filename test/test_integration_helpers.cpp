// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Unit tests for haze::test::negacyclic_conv_ref, the host-side O(N^2)
// oracle used by the NTT.Mul.INTT integration cases. The integration
// tests compare the FHETCH simulator output against this helper, so a
// silent bug in the helper would mask a matching bug on the haze side.
// These tests pin the helper's defining property — the X^N = -1 sign
// flip on wrap that distinguishes negacyclic from cyclic convolution —
// without involving FHETCH or haze.

#include "integration_helpers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

// Small ring dim and prime keep the assertions readable and the
// arithmetic obvious; the helper itself imposes no size constraint.
constexpr std::size_t kN = 8;
constexpr uint64_t kQ = 257;

} // namespace

TEST_CASE("negacyclic_conv_ref: multiplication by X rotates with sign flip", "[unit]") {
    // X is the polynomial e_1 = [0, 1, 0, ..., 0]. In Z_q[X] / (X^N + 1):
    //   a(X) * X = a_0*X + a_1*X^2 + ... + a_{N-1}*X^N
    //            = -a_{N-1} + a_0*X + a_1*X^2 + ... + a_{N-2}*X^{N-1}.
    // So c[0] = (q - a_{N-1}) mod q and c[k] = a[k-1] for k >= 1. Every
    // input slot is nonzero so the helper's `if (a[i] == 0) continue;`
    // skip never short-circuits the wrap site under test.
    std::vector<uint64_t> a(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        a[i] = ((i * 13ULL) + 1ULL) % kQ;
    }
    std::vector<uint64_t> x(kN, 0);
    x[1] = 1;

    const auto c = haze::test::negacyclic_conv_ref(a, x, kQ);

    REQUIRE(c[0] == kQ - a[kN - 1]);
    for (std::size_t k = 1; k < kN; ++k) {
        REQUIRE(c[k] == a[k - 1]);
    }
}

TEST_CASE("negacyclic_conv_ref: monomial product wraps with sign flip at X^N = -1", "[unit]") {
    // For every (i, j), e_i * e_j = X^{i+j}. In Z_q[X] / (X^N + 1):
    //   i + j <  N: c[i+j]     = 1,   others 0.
    //   i + j >= N: c[i+j - N] = q-1, others 0.
    // The sign flip on wrap is the smoking gun distinguishing negacyclic
    // from cyclic convolution — under cyclic the wrap site would still
    // be 1, not q-1. Sweeping the full (i, j) grid covers no-wrap,
    // boundary (i+j == N), and deep-wrap (i+j == 2N-2) in one pass.
    for (std::size_t i = 0; i < kN; ++i) {
        for (std::size_t j = 0; j < kN; ++j) {
            std::vector<uint64_t> ei(kN, 0);
            std::vector<uint64_t> ej(kN, 0);
            ei[i] = 1;
            ej[j] = 1;

            const auto c = haze::test::negacyclic_conv_ref(ei, ej, kQ);
            const std::size_t wrap_idx = (i + j) % kN;
            const uint64_t expected = ((i + j) >= kN) ? (kQ - 1) : 1ULL;

            for (std::size_t m = 0; m < kN; ++m) {
                CAPTURE(i, j, m);
                REQUIRE(c[m] == (m == wrap_idx ? expected : 0ULL));
            }
        }
    }
}
