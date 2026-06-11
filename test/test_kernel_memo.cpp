// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Kernel-memoization core tests, including THE acceptance gate for the
// whole feature: the trace a memoized second call produces must be
// byte-identical to a cold recording of the same program (modulo the
// wall-clock "# Generated:" comment). A memoized kernel is a record-time
// optimization only — the .fhetch it yields may never differ.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <niobium/compiler.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kN = 4096;

// Read the .fhetch trace of the current program dir, dropping the
// wall-clock comment line.
std::string normalized_trace() {
    const auto dir = niobium::compiler().get_program_directory();
    std::ifstream in(dir / "haze.fhetch");
    REQUIRE(in.good());
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line))
        if (!line.starts_with("# Generated:"))
            out << line << '\n';
    return out.str();
}

struct UploadedPair {
    void *a = nullptr;
    void *b = nullptr;
};

UploadedPair upload_pair(uint64_t q, uint64_t seed) {
    const auto va = haze::test::make_residue(q, seed, kN);
    const auto vb = haze::test::make_residue(q, seed + 1, kN);
    UploadedPair p;
    REQUIRE(hazeMalloc(&p.a, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&p.b, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(p.a, va.data(), kN * sizeof(uint64_t), HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(p.b, vb.data(), kN * sizeof(uint64_t), HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_SUCCESS);
    return p;
}

// One bracketed "kadd" call: dst = a + b under mod_idx 0. Returns the
// disposition Begin reported.
hazeKernelDisposition kadd(uint64_t q, void *dst, void *a, void *b) {
    const void *in_a[] = {a};
    const void *in_b[] = {b};
    const hazeKernelInput inputs[] = {{in_a, &q, 1}, {in_b, &q, 1}};
    const uint8_t key[] = {0x42}; // scalars/moduli constant across calls
    hazeKernelDisposition disposition{};
    REQUIRE(hazeKernelBegin("kadd", 0x4242, key, sizeof(key), inputs, 2, &disposition) ==
            HAZE_SUCCESS);
    if (disposition == HAZE_KERNEL_RECORD)
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    void *const out_res[] = {dst};
    const hazeKernelOutput outputs[] = {{out_res, &q, 1}};
    REQUIRE(hazeKernelEnd(outputs, 1) == HAZE_SUCCESS);
    return disposition;
}

// The program under test: two kadd calls on distinct inputs, written
// (not replayed) so the .fhetch can be compared byte-for-byte.
std::string run_program(bool memo, bool *second_replayed = nullptr) {
    const uint64_t q = haze::test::setup_integration_compute_config(kN);
    REQUIRE(hazeSetKernelMemo(memo ? 1 : 0) == HAZE_SUCCESS);

    const UploadedPair first = upload_pair(q, 1000);
    const UploadedPair second = upload_pair(q, 2000);
    void *dst1 = nullptr;
    void *dst2 = nullptr;
    REQUIRE(hazeMalloc(&dst1, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst2, kN * sizeof(uint64_t)) == HAZE_SUCCESS);

    const hazeKernelDisposition d1 = kadd(q, dst1, first.a, first.b);
    REQUIRE(d1 == HAZE_KERNEL_RECORD); // fresh cache either way
    const hazeKernelDisposition d2 = kadd(q, dst2, second.a, second.b);
    if (second_replayed != nullptr)
        *second_replayed = (d2 == HAZE_KERNEL_REPLAY);

    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    return normalized_trace();
}

} // namespace

TEST_CASE("memoized kernel replay writes a byte-identical trace", "[integration][kernelmemo]") {
    bool replayed = false;
    const std::string cold = run_program(/*memo=*/false);
    const std::string memoized = run_program(/*memo=*/true, &replayed);
    REQUIRE(replayed); // the second call really did skip its body
    REQUIRE(!cold.empty());
    REQUIRE(cold == memoized);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("memoized kernel results decrypt to the right values", "[integration][kernelmemo]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kN);
    REQUIRE(hazeSetKernelMemo(1) == HAZE_SUCCESS);

    const UploadedPair first = upload_pair(q, 3000);
    const UploadedPair second = upload_pair(q, 4000);
    void *dst1 = nullptr;
    void *dst2 = nullptr;
    REQUIRE(hazeMalloc(&dst1, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst2, kN * sizeof(uint64_t)) == HAZE_SUCCESS);

    REQUIRE(kadd(q, dst1, first.a, first.b) == HAZE_KERNEL_RECORD);
    REQUIRE(kadd(q, dst2, second.a, second.b) == HAZE_KERNEL_REPLAY);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    const auto check = [&](void *dst, uint64_t seed) {
        const auto va = haze::test::make_residue(q, seed, kN);
        const auto vb = haze::test::make_residue(q, seed + 1, kN);
        std::vector<uint64_t> got(kN);
        REQUIRE(hazeMemcpy(got.data(), dst, kN * sizeof(uint64_t), HAZE_MEMCPY_DEVICE_TO_HOST) ==
                HAZE_SUCCESS);
        for (std::size_t i = 0; i < kN; ++i)
            REQUIRE(got[i] == (va[i] + vb[i]) % q);
    };
    check(dst1, 3000);
    check(dst2, 4000); // the replayed instance computed on ITS inputs
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("kernel protocol misuse and closed-body violations are rejected",
          "[integration][kernelmemo]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kN);
    REQUIRE(hazeSetKernelMemo(1) == HAZE_SUCCESS);

    // End/Abort without an open bracket.
    REQUIRE(hazeKernelEnd(nullptr, 0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeKernelAbort() == HAZE_ERROR_INVALID_VALUE);

    // Unrecorded input: kernels can't promote foreign buffers.
    void *cold_buf = nullptr;
    REQUIRE(hazeMalloc(&cold_buf, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    {
        const void *res[] = {cold_buf};
        const hazeKernelInput inputs[] = {{res, &q, 1}};
        hazeKernelDisposition disposition{};
        REQUIRE(hazeKernelBegin("unbound", 1, nullptr, 0, inputs, 1, &disposition) ==
                HAZE_ERROR_SOURCE_UNAVAILABLE);
    }

    // Closed-body violation: the body writes a buffer that is neither
    // an input nor a declared output.
    const UploadedPair pair = upload_pair(q, 5000);
    void *declared = nullptr;
    void *smuggled = nullptr;
    REQUIRE(hazeMalloc(&declared, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&smuggled, kN * sizeof(uint64_t)) == HAZE_SUCCESS);
    {
        const void *in_a[] = {pair.a};
        const void *in_b[] = {pair.b};
        const hazeKernelInput inputs[] = {{in_a, &q, 1}, {in_b, &q, 1}};
        hazeKernelDisposition disposition{};
        REQUIRE(hazeKernelBegin("leaky", 2, nullptr, 0, inputs, 2, &disposition) == HAZE_SUCCESS);
        REQUIRE(disposition == HAZE_KERNEL_RECORD);
        REQUIRE(hazeAdd(smuggled, pair.a, pair.b, 0, nullptr) == HAZE_SUCCESS);
        void *const out_res[] = {declared};
        const hazeKernelOutput outputs[] = {{out_res, &q, 1}};
        REQUIRE(hazeKernelEnd(outputs, 1) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    }

    // Nesting is reserved: a Begin inside an open bracket is rejected.
    {
        const void *in_a[] = {pair.a};
        const hazeKernelInput inputs[] = {{in_a, &q, 1}};
        hazeKernelDisposition disposition{};
        REQUIRE(hazeKernelBegin("outer", 3, nullptr, 0, inputs, 1, &disposition) == HAZE_SUCCESS);
        hazeKernelDisposition inner{};
        REQUIRE(hazeKernelBegin("inner", 4, nullptr, 0, inputs, 1, &inner) ==
                HAZE_ERROR_NOT_SUPPORTED);
        REQUIRE(hazeKernelAbort() == HAZE_SUCCESS);
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("validation mode catches a structurally diverging body", "[integration][kernelmemo]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kN);
    REQUIRE(hazeSetKernelMemo(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetKernelValidate(1) == HAZE_SUCCESS);

    const UploadedPair pair = upload_pair(q, 6000);
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kN * sizeof(uint64_t)) == HAZE_SUCCESS);

    // A "kernel" whose body shape depends on hidden state: one add on
    // the first call, two on the second — same key both times.
    static int call_count = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    call_count = 0;
    const auto shifty = [&]() -> hazeError_t {
        const void *in_a[] = {pair.a};
        const void *in_b[] = {pair.b};
        const hazeKernelInput inputs[] = {{in_a, &q, 1}, {in_b, &q, 1}};
        hazeKernelDisposition disposition{};
        if (const hazeError_t err =
                hazeKernelBegin("shifty", 7, nullptr, 0, inputs, 2, &disposition);
            err != HAZE_SUCCESS)
            return err;
        if (disposition == HAZE_KERNEL_RECORD) {
            ++call_count;
            if (hazeAdd(dst, pair.a, pair.b, 0, nullptr) != HAZE_SUCCESS)
                return hazeGetLastError();
            if (call_count > 1 && hazeAdd(dst, dst, pair.b, 0, nullptr) != HAZE_SUCCESS)
                return hazeGetLastError();
        }
        void *const out_res[] = {dst};
        const hazeKernelOutput outputs[] = {{out_res, &q, 1}};
        return hazeKernelEnd(outputs, 1);
    };

    REQUIRE(shifty() == HAZE_SUCCESS);                 // records
    REQUIRE(shifty() == HAZE_ERROR_KERNEL_VALIDATION); // re-trace diverges
    REQUIRE(hazeSetKernelValidate(0) == HAZE_SUCCESS); // restore default
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
