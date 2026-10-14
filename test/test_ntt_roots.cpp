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
// Caller-chosen NTT roots (hazeFheParams::twiddle_generators): stored per
// modulus by hazeConfigureDevice and carried by every recorded hazeNTT /
// hazeINTT and their Mrp forms. The [unit] cases read the frozen config and the
// written trace; the [integration] cases compare values with a definition-level
// reference NTT. Cases that upload coefficient-form bytes skip under
// HAZE_TRANSPORT_HW (see skip_if_hw_coefficient_input). The evaluation-form
// INTT case runs there too and accepts either outcome a hardware-format target
// may have: honouring the root, or refusing a trace that carries one.

#include "core/config.hpp"
#include "integration_helpers.hpp"
#include "mod_arith_ref.hpp"

#include <algorithm>
#include <bit>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ma = niobium::mod_arith;

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
// The suite's NTT-friendly primes (q ≡ 1 mod 2N) for N=4096.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;
// An odd Galois element, so X -> X^k is an automorphism of Z_q[X]/(X^N + 1).
constexpr uint64_t kGaloisElement = 5;

// The least odd power of a primitive 2N-th root of unity mod q: the minimal
// root, which a trace consumer derives for a transform that records none.
uint64_t minimal_root(uint64_t q) {
    const uint64_t psi = ma::qnr_root(q, kRingDim);
    const uint64_t psi_squared = ma::mulmod(psi, psi, q);
    uint64_t power = psi;
    uint64_t least = psi;
    for (uint64_t i = 1; i < kRingDim; ++i) {
        power = ma::mulmod(power, psi_squared, q);
        least = std::min(least, power);
    }
    return least;
}

// A caller-chosen root distinct from the minimal one, so a transform recorded
// without it produces different values.
uint64_t chosen_root(uint64_t q) {
    const uint64_t psi = ma::qnr_root(q, kRingDim);
    REQUIRE(psi != minimal_root(q));
    return psi;
}

// Negacyclic NTT from its definition: slot j holds a(psi^(2*brv(j) + 1)) mod q,
// brv reversing log2(N) bits (the sr_ntt_with_root contract). O(N^2), about
// 0.07 s at N = 4096 in Release.
std::vector<uint64_t> reference_ntt(const std::vector<uint64_t> &a, uint64_t q, uint64_t psi) {
    const auto log_n = static_cast<unsigned>(std::countr_zero(a.size()));
    std::vector<uint64_t> out(a.size());
    for (std::size_t j = 0; j < a.size(); ++j) {
        uint64_t reversed = 0;
        for (unsigned bit = 0; bit < log_n; ++bit)
            reversed |= ((j >> bit) & 1U) << (log_n - 1U - bit);
        const uint64_t x = ma::powmod(psi, (2 * reversed) + 1, q);
        uint64_t acc = 0; // Horner: sum of a[k] * x^k
        for (std::size_t k = a.size(); k-- > 0;)
            acc = haze::test::add_mod(ma::mulmod(acc, x, q), a[k], q);
        out[j] = acc;
    }
    return out;
}

// a(X) -> a(X^k) in Z_q[X]/(X^N + 1) for odd k: coefficient i moves to i*k mod
// 2N, negated when that lands at or above N.
std::vector<uint64_t> galois_automorphism(const std::vector<uint64_t> &a, uint64_t q, uint64_t k) {
    const std::size_t n = a.size();
    std::vector<uint64_t> out(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t target = (i * k) % (2 * n);
        out[target % n] = (target < n || a[i] == 0) ? a[i] : q - a[i];
    }
    return out;
}

// Reset and configure `moduli` with `roots` (empty for none) on `target`, then
// init the bridge so MRP outputs can be read back. An empty `program_dir` keeps
// the default <cwd>/<program name>.
void configure(const std::vector<uint64_t> &moduli, const std::vector<uint64_t> &roots,
               const char *target, const std::string &program_dir = {}) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeFheParams fhe = {.ring_dim = kRingDim,
                               .moduli = moduli.data(),
                               .moduli_count = moduli.size(),
                               .twiddle_generators = roots.data(),
                               .twiddle_count = roots.size()};
    const hazeReplayConfig replay = {.target = target,
                                     .program_directory =
                                         program_dir.empty() ? nullptr : program_dir.c_str(),
                                     .reduced_noise = 1};
    REQUIRE(hazeConfigureDevice(&fhe, &replay) == HAZE_SUCCESS);
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, moduli[0]) == HAZE_SUCCESS);
}

void tag_outputs(const std::vector<void *> &ptrs) {
    for (void *p : ptrs)
        REQUIRE(hazeTagOutput(p) == HAZE_SUCCESS);
}

std::vector<uint64_t> read_back(const void *ptr) {
    std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(got.data(), ptr, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    return got;
}

// Slot-by-slot so a failure names the first differing slot, not two 4096-entry vectors.
void require_equal(const std::vector<uint64_t> &got, const std::vector<uint64_t> &expected) {
    REQUIRE(got.size() == expected.size());
    const auto first = std::mismatch(got.begin(), got.end(), expected.begin()).first;
    INFO("first mismatch at slot " << (first - got.begin()));
    REQUIRE(first == got.end());
}

std::string slurp(const fs::path &path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t count_occurrences(const std::string &haystack, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// The one .fhetch trace hazeWriteProgram left in `dir`.
std::string read_trace(const fs::path &dir) {
    std::vector<fs::path> traces;
    for (const auto &entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".fhetch")
            traces.push_back(entry.path());
    REQUIRE(traces.size() == 1);
    return slurp(traces.front());
}

// Records seven transforms into a fresh temp program dir named `dir_name` and
// returns the written trace: hazeNTT at kQ0, hazeINTT at kQ1, hazeNTTMrp over
// {kQ0, kQ1, aux prime}, and hazeINTTMrp over {kQ0, kQ1}. The non-local target
// keeps this off the simulator path; hazeWriteProgram never replays.
std::string record_transforms_trace(const std::string &dir_name,
                                    const std::vector<uint64_t> &roots) {
    const fs::path dir = fs::temp_directory_path() / dir_name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::vector<uint64_t> moduli = {kQ0, kQ1};
    configure(moduli, roots, "FUNC_SIM", dir.string());
    const uint64_t aux = haze::fhe_params().aux_modulus();
    REQUIRE(aux != 0);

    const std::vector<void *> d_srp = haze::test::allocate_and_h2d_residues(
        {haze::test::make_residue(kQ0, 11, kRingDim), haze::test::make_residue(kQ1, 12, kRingDim)});
    const std::vector<void *> d_srp_out = haze::test::allocate_dst_residues(2, kBytes);
    REQUIRE(hazeNTT(d_srp_out[0], d_srp[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_srp_out[1], d_srp[1], 1, nullptr) == HAZE_SUCCESS);

    const std::vector<uint64_t> ntt_base = {kQ0, kQ1, aux};
    const std::vector<std::vector<uint64_t>> ntt_in = {haze::test::make_residue(kQ0, 21, kRingDim),
                                                       haze::test::make_residue(kQ1, 21, kRingDim),
                                                       haze::test::make_residue(aux, 21, kRingDim)};
    const std::vector<void *> d_ntt_in = haze::test::allocate_and_h2d_residues(ntt_in, ntt_base);
    const std::vector<void *> d_ntt_out = haze::test::allocate_dst_residues(3, kBytes);
    const std::vector<const void *> ntt_view = haze::test::to_const(d_ntt_in);
    REQUIRE(hazeNTTMrp(d_ntt_out.data(), ntt_view.data(), ntt_base.data(), ntt_base.size(),
                       nullptr) == HAZE_SUCCESS);

    const std::vector<void *> d_intt_in = haze::test::allocate_and_h2d_residues(
        {haze::test::make_residue(kQ0, 31, kRingDim), haze::test::make_residue(kQ1, 32, kRingDim)},
        moduli);
    const std::vector<void *> d_intt_out = haze::test::allocate_dst_residues(2, kBytes);
    const std::vector<const void *> intt_view = haze::test::to_const(d_intt_in);
    REQUIRE(hazeINTTMrp(d_intt_out.data(), intt_view.data(), moduli.data(), moduli.size(),
                        nullptr) == HAZE_SUCCESS);

    tag_outputs(d_srp_out);
    tag_outputs(d_ntt_out);
    tag_outputs(d_intt_out);
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    for (const auto *group : {&d_srp, &d_srp_out, &d_ntt_in, &d_ntt_out, &d_intt_in, &d_intt_out})
        haze::test::free_all_residues(*group);
    const std::string trace = read_trace(dir);
    fs::remove_all(dir, ec);
    return trace;
}

constexpr std::size_t kRecordedTransforms = 7;

std::size_t count_transforms(const std::string &trace) {
    return count_occurrences(trace, "\nsr_ntt ") + count_occurrences(trace, "\nsr_intt ");
}

// omega is the trailing operand of an sr_ntt / sr_intt line.
std::string omega_operand(uint64_t root) {
    return ", omega=" + std::to_string(root) + "\n";
}

// Uploads `residues` under `base`, runs hazeNTTMrp then hazeINTTMrp over
// `base`, and checks the evaluation form against reference_ntt at
// expected_roots[i] and the round trip against the input.
void check_mrp_transforms(const std::vector<uint64_t> &base,
                          const std::vector<std::vector<uint64_t>> &residues,
                          const std::vector<uint64_t> &expected_roots) {
    const std::vector<void *> d_x = haze::test::allocate_and_h2d_residues(residues, base);
    const std::vector<void *> d_eval = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<void *> d_back = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> x_view = haze::test::to_const(d_x);
    REQUIRE(hazeNTTMrp(d_eval.data(), x_view.data(), base.data(), base.size(), nullptr) ==
            HAZE_SUCCESS);
    const std::vector<const void *> eval_view = haze::test::to_const(d_eval);
    REQUIRE(hazeINTTMrp(d_back.data(), eval_view.data(), base.data(), base.size(), nullptr) ==
            HAZE_SUCCESS);
    tag_outputs(d_eval);
    tag_outputs(d_back);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (std::size_t i = 0; i < base.size(); ++i) {
        INFO("residue " << i << " (mod " << base[i] << ")");
        require_equal(read_back(d_eval[i]), reference_ntt(residues[i], base[i], expected_roots[i]));
        require_equal(read_back(d_back[i]), residues[i]);
    }
    haze::test::free_all_residues(d_x);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_back);
}

} // namespace

TEST_CASE("hazeConfigureDevice stores one caller root per modulus", "[unit]") {
    // Any odd power of a primitive 2N-th root is one too.
    const uint64_t psi0 = ma::qnr_root(kQ0, kRingDim);
    const uint64_t psi1 = ma::powmod(ma::qnr_root(kQ1, kRingDim), 3, kQ1);
    configure({kQ0, kQ1}, {psi0, psi1}, nullptr);
    const haze::FheParams &params = haze::fhe_params();
    REQUIRE(params.ntt_root(kQ0) == psi0);
    REQUIRE(params.ntt_root(kQ1) == psi1);
    // The generated aux prime has no caller root, so it keeps the minimal one.
    REQUIRE(params.aux_modulus() != 0);
    REQUIRE_FALSE(params.ntt_root(params.aux_modulus()).has_value());
    REQUIRE_FALSE(params.ntt_root(kQ2).has_value()); // not in the chain
}

TEST_CASE("hazeConfigureDevice without roots stores no NTT root", "[unit]") {
    configure({kQ0, kQ1}, {}, nullptr);
    const haze::FheParams &params = haze::fhe_params();
    REQUIRE_FALSE(params.ntt_root(kQ0).has_value());
    REQUIRE_FALSE(params.ntt_root(kQ1).has_value());
    REQUIRE_FALSE(params.ntt_root(params.aux_modulus()).has_value());
}

TEST_CASE("configured roots ride every recorded NTT and INTT", "[unit]") {
    const uint64_t psi0 = chosen_root(kQ0);
    const uint64_t psi1 = chosen_root(kQ1);
    const std::string trace = record_transforms_trace("haze_ntt_roots_rooted", {psi0, psi1});
    CAPTURE(trace);
    REQUIRE(count_transforms(trace) == kRecordedTransforms);
    // kQ0: hazeNTT and one residue of each Mrp call; kQ1 likewise with hazeINTT.
    REQUIRE(count_occurrences(trace, omega_operand(psi0)) == 3);
    REQUIRE(count_occurrences(trace, omega_operand(psi1)) == 3);
    // The aux prime's residue has no caller root and records none.
    REQUIRE(count_occurrences(trace, ", omega=") == 6);
}

TEST_CASE("without configured roots no recorded NTT or INTT carries one", "[unit]") {
    const std::string trace = record_transforms_trace("haze_ntt_roots_rootless", {});
    CAPTURE(trace);
    REQUIRE(count_transforms(trace) == kRecordedTransforms);
    REQUIRE(count_occurrences(trace, "omega=") == 0);
}

TEST_CASE("hazeNTT with a configured root matches the reference NTT at that root",
          "[integration]") {
    haze::test::skip_if_hw_coefficient_input();
    const uint64_t psi = chosen_root(kQ0);
    configure({kQ0}, {psi}, haze::test::target_from_env());
    const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 4242, kRingDim);
    const std::vector<void *> d_a = haze::test::allocate_and_h2d_residues({a});
    const std::vector<void *> d_out = haze::test::allocate_dst_residues(2, kBytes);
    REQUIRE(hazeNTT(d_out[0], d_a[0], 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeINTT(d_out[1], d_out[0], 0, nullptr) == HAZE_SUCCESS);
    tag_outputs(d_out);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    const std::vector<uint64_t> eval = read_back(d_out[0]);
    require_equal(eval, reference_ntt(a, kQ0, psi));
    // The check discriminates: the minimal root evaluates elsewhere.
    REQUIRE(eval != reference_ntt(a, kQ0, minimal_root(kQ0)));
    require_equal(read_back(d_out[1]), a);
    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_out);
}

TEST_CASE("hazeNTT without a configured root matches the reference NTT at the minimal root",
          "[integration]") {
    // Validates the reference against the rootless path the suite already
    // exercises.
    haze::test::skip_if_hw_coefficient_input();
    configure({kQ0}, {}, haze::test::target_from_env());
    const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 4242, kRingDim);
    const std::vector<void *> d_a = haze::test::allocate_and_h2d_residues({a});
    const std::vector<void *> d_out = haze::test::allocate_dst_residues(1, kBytes);
    REQUIRE(hazeNTT(d_out[0], d_a[0], 0, nullptr) == HAZE_SUCCESS);
    tag_outputs(d_out);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    require_equal(read_back(d_out[0]), reference_ntt(a, kQ0, minimal_root(kQ0)));
    haze::test::free_all_residues(d_a);
    haze::test::free_all_residues(d_out);
}

TEST_CASE("hazeINTT with a configured root inverts the reference NTT at that root",
          "[integration]") {
    const uint64_t psi = chosen_root(kQ0);
    configure({kQ0}, {psi}, haze::test::target_from_env());
    const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 777, kRingDim);
    const std::vector<void *> d_eval =
        haze::test::allocate_and_h2d_residues({reference_ntt(a, kQ0, psi)});
    const std::vector<void *> d_out = haze::test::allocate_dst_residues(1, kBytes);
    REQUIRE(hazeINTT(d_out[0], d_eval[0], 0, nullptr) == HAZE_SUCCESS);
    tag_outputs(d_out);
    const hazeError_t flushed = hazeFlush();
    if (haze::test::hw_target_active()) {
        // A hardware-format target either honours the root or, when it
        // transforms only with the minimal one, refuses the trace.
        REQUIRE((flushed == HAZE_SUCCESS || flushed == HAZE_ERROR_INTERNAL));
    } else {
        REQUIRE(flushed == HAZE_SUCCESS);
    }
    if (flushed == HAZE_SUCCESS)
        require_equal(read_back(d_out[0]), a);
    else
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INTERNAL);
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_out);
}

TEST_CASE("hazeNTTMrp and hazeINTTMrp use each residue's configured root", "[integration]") {
    haze::test::skip_if_hw_coefficient_input();
    const std::vector<uint64_t> base = {kQ0, kQ1, kQ2};
    std::vector<uint64_t> roots;
    std::vector<std::vector<uint64_t>> x;
    for (std::size_t i = 0; i < base.size(); ++i) {
        roots.push_back(chosen_root(base[i]));
        x.push_back(haze::test::make_residue(base[i], 900 + i, kRingDim));
    }
    configure(base, roots, haze::test::target_from_env());
    check_mrp_transforms(base, x, roots);
}

TEST_CASE("hazeNTTMrp over rooted primes and the aux prime uses each residue's own root",
          "[integration]") {
    haze::test::skip_if_hw_coefficient_input();
    const std::vector<uint64_t> roots = {chosen_root(kQ0), chosen_root(kQ1)};
    configure({kQ0, kQ1}, roots, haze::test::target_from_env());
    const uint64_t aux = haze::fhe_params().aux_modulus();
    REQUIRE(aux != 0);
    const std::vector<uint64_t> base = {kQ0, kQ1, aux};
    std::vector<std::vector<uint64_t>> x(base.size());
    for (std::size_t i = 0; i < base.size(); ++i)
        x[i] = haze::test::make_residue(base[i], 1300 + i, kRingDim);
    // The aux prime has no caller root, so its residue uses the minimal one.
    check_mrp_transforms(base, x, {roots[0], roots[1], minimal_root(aux)});
}

TEST_CASE("hazeAutomorphMrp on evaluation-form data is independent of the configured root",
          "[integration]") {
    // The input is uploaded as the reference NTT at each caller root, so it is
    // evaluation form and the trace records no transform.
    const std::vector<uint64_t> base = {kQ0, kQ1};
    const std::vector<uint64_t> roots = {chosen_root(kQ0), chosen_root(kQ1)};
    configure(base, roots, haze::test::target_from_env());
    std::vector<std::vector<uint64_t>> a;
    std::vector<std::vector<uint64_t>> eval;
    for (std::size_t i = 0; i < base.size(); ++i) {
        a.push_back(haze::test::make_residue(base[i], 1700 + i, kRingDim));
        eval.push_back(reference_ntt(a[i], base[i], roots[i]));
    }
    const std::vector<void *> d_eval = haze::test::allocate_and_h2d_residues(eval, base);
    const std::vector<void *> d_out = haze::test::allocate_dst_residues(base.size(), kBytes);
    const std::vector<const void *> eval_view = haze::test::to_const(d_eval);
    REQUIRE(hazeAutomorphMrp(d_out.data(), eval_view.data(), kGaloisElement, base.data(),
                             base.size(), nullptr) == HAZE_SUCCESS);
    tag_outputs(d_out);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    for (std::size_t i = 0; i < base.size(); ++i) {
        INFO("residue " << i << " (mod " << base[i] << ")");
        const std::vector<uint64_t> rotated = galois_automorphism(a[i], base[i], kGaloisElement);
        require_equal(read_back(d_out[i]), reference_ntt(rotated, base[i], roots[i]));
    }
    haze::test::free_all_residues(d_eval);
    haze::test::free_all_residues(d_out);
}
