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
// Montgomery + bit-reversal data-format tests.
//
// Montgomery form is a temporary internal encoding: a bijection
// x <-> x * R mod q (R fixed by the executor) computed with exact integer arithmetic, decoded back
// by the replay engine before results are read. It must therefore never
// change results — the transport tests assert byte-exact equality against
// the same oracles as ordinary mode. The cases in this file run without a
// compiler install: they cover the mod_arith helpers, the config/API
// resolution rules, the local-target rejection, and record-time trace /
// replay-JSON / captured-input assertions via hazeWriteProgram().

#include "core/config.hpp"
#include "integration_helpers.hpp"
#include "mod_arith_ref.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <niobium/compiler.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Captured-input iteration helper. Declared in libnbfhetch's private
// compiler_internal.h; haze_tests links the archive directly, so declaring
// the signature here resolves against the same symbol.
namespace niobium::detail {
void for_each_captured_input(const std::function<void(const niobium::CapturedInputRecord &)> &cb);
} // namespace niobium::detail

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr size_t kBytes = kRingDim * sizeof(uint64_t);

// Same NTT-friendly primes (q ≡ 1 mod 2N, N=4096) as test_basis_convert.cpp.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

// Independent reference for x * R mod q (R the executor radix), against which mod_arith's
// implementation is cross-checked.
uint64_t ref_to_montgomery(uint64_t x, uint64_t q) {
    __uint128_t wide = x % q;
    wide <<= 64;
    return static_cast<uint64_t>(wide % q);
}

// Montgomery multiplication as the FUNC_SIM executor performs it:
// (a * b) * R^-1 mod q. Used to simulate the executor's 4-op switchmodulus
// sequence end-to-end.
uint64_t ref_montgomery_mul(uint64_t a, uint64_t b, uint64_t q) {
    const uint64_t prod = niobium::mod_arith::mulmod(a, b, q);
    const uint64_t r_inv = niobium::mod_arith::modinv_prime(niobium::mod_arith::montgomery_r(q), q);
    return niobium::mod_arith::mulmod(prod, r_inv, q);
}

std::string slurp(const std::filesystem::path &path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Extract the JSON value text following `"key":` (up to the next comma or
// newline). String search keeps the test free of a JSON library dependency.
std::string json_value_text(const std::string &doc, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    const size_t key_pos = doc.find(needle);
    REQUIRE(key_pos != std::string::npos);
    const size_t colon = doc.find(':', key_pos);
    REQUIRE(colon != std::string::npos);
    size_t end = doc.find_first_of(",\n}", colon);
    REQUIRE(end != std::string::npos);
    std::string value = doc.substr(colon + 1, end - colon - 1);
    // Trim surrounding whitespace.
    const size_t first = value.find_first_not_of(" \t");
    const size_t last = value.find_last_not_of(" \t");
    REQUIRE(first != std::string::npos);
    return value.substr(first, last - first + 1);
}

// RAII guard so env-var manipulation can't leak into other test cases.
class EnvGuard {
  public:
    explicit EnvGuard(std::vector<const char *> names) : names_(std::move(names)) {
        for (const char *name : names_) {
            const char *old = std::getenv(name);
            saved_.emplace_back(old != nullptr, old != nullptr ? old : "");
            // setenv/unsetenv are POSIX; <cstdlib> is the closest standard
            // header. Return value discarded: only fails on a null/invalid
            // name, which these literals are not.
            static_cast<void>(::unsetenv(name)); // NOLINT(misc-include-cleaner)
        }
    }
    ~EnvGuard() {
        for (size_t i = 0; i < names_.size(); ++i) {
            if (saved_[i].first) {
                static_cast<void>(::setenv(names_[i], saved_[i].second.c_str(),
                                           1)); // NOLINT(misc-include-cleaner)
            } else {
                static_cast<void>(::unsetenv(names_[i]));
            }
        }
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

  private:
    std::vector<const char *> names_;
    std::vector<std::pair<bool, std::string>> saved_;
};

// Record one scalar-multiply (and optionally a mod-down) into a uniquely
// named program dir with the current data-format settings, then write
// the project without replaying. Returns the program directory.
std::filesystem::path record_program(const std::string &program_name, bool with_mod_down) {
    REQUIRE(hazeSetProgramInfo(program_name.c_str(), "0.1", "data-format test") == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    std::vector<uint64_t> vals(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        vals[i] = (i * 7 + 13) % kQ0;

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(dst, src, 7, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);

    if (with_mod_down) {
        void *s0 = nullptr;
        void *s1 = nullptr;
        void *d0 = nullptr;
        REQUIRE(hazeMalloc(&s0, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&s1, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMalloc(&d0, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(s0, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(s1, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

        const uint64_t src_base[] = {kQ0, kQ1};
        const uint64_t rescale_base[] = {kQ1};
        const void *src_polys[] = {s0, s1};
        void *dst_polys[] = {d0};
        hazeModDownParams p{};
        p.src_base = src_base;
        p.src_base_len = 2;
        p.rescale_base = rescale_base;
        p.rescale_base_len = 1;
        REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(d0) == HAZE_SUCCESS);
    }

    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    return std::filesystem::path{program_name};
}

} // namespace

TEST_CASE("mod_arith: montgomery encode/decode is an exact bijection", "[unit][hwfmt]") {
    using namespace niobium::mod_arith;
    for (uint64_t q : {kQ0, kQ1, kQ2}) {
        REQUIRE(to_montgomery(uint64_t{0}, q) == 0);
        REQUIRE(to_montgomery(uint64_t{1}, q) == montgomery_r(q));
        for (uint64_t v : {uint64_t{1}, uint64_t{2}, uint64_t{12345678901234ULL}, q - 1, q + 5}) {
            const uint64_t enc = to_montgomery(v, q);
            REQUIRE(enc == ref_to_montgomery(v, q));
            REQUIRE(from_montgomery(enc, q) == v % q);
        }
    }
}

TEST_CASE("mod_arith: bulk vector encode matches scalar encode", "[unit][hwfmt]") {
    using namespace niobium::mod_arith;
    std::vector<uint64_t> vals(64);
    for (size_t i = 0; i < vals.size(); ++i)
        vals[i] = (i * 9973) + 17;
    std::vector<uint64_t> enc = vals;
    to_montgomery(enc, kQ0);
    for (size_t i = 0; i < vals.size(); ++i)
        REQUIRE(enc[i] == to_montgomery(vals[i], kQ0));
    from_montgomery(enc, kQ0);
    REQUIRE(enc == vals);
}

TEST_CASE("mod_arith: bit reversal matches reference and is an involution", "[unit][hwfmt]") {
    using namespace niobium::mod_arith;
    // n=8 reference permutation.
    std::vector<uint64_t> v8 = {0, 1, 2, 3, 4, 5, 6, 7};
    apply_bit_reversal(v8);
    REQUIRE(v8 == std::vector<uint64_t>{0, 4, 2, 6, 1, 5, 3, 7});

    for (uint64_t n : {uint64_t{2048}, uint64_t{4096}, uint64_t{65536}}) {
        std::vector<uint64_t> v(n);
        for (uint64_t i = 0; i < n; ++i)
            v[i] = i;
        std::vector<uint64_t> once = v;
        apply_bit_reversal(once);
        unsigned log2n = 0;
        while ((1ULL << log2n) < n)
            ++log2n;
        for (uint64_t i = 0; i < n; ++i)
            REQUIRE(once[bit_reverse_index(i, log2n)] == i);
        apply_bit_reversal(once);
        REQUIRE(once == v);
    }
}

TEST_CASE("mod_arith: switchmodulus immediates (ordinary and montgomery)", "[unit][hwfmt]") {
    using namespace niobium::mod_arith;
    const uint64_t q = kQ1;
    const uint64_t p = kQ0;
    const uint64_t half_q = (q - 1) / 2;

    const auto ord = compute_switchmodulus_immediates(q, p, /*montgomery=*/false);
    REQUIRE(ord.imm[0] == 1);
    REQUIRE(ord.imm[1] == half_q);
    REQUIRE(ord.imm[2] == 1);
    REQUIRE(ord.imm[3] == p - (half_q % p));

    const auto mont = compute_switchmodulus_immediates(q, p, /*montgomery=*/true);
    REQUIRE(mont.imm[0] == 1);
    REQUIRE(mont.imm[1] == half_q);
    REQUIRE(mont.imm[2] == mulmod(montgomery_r(p), montgomery_r(p), p)); // R^2 mod p
    REQUIRE(mont.imm[3] == p - mulmod(montgomery_r(p), half_q % p, p));  // -(R*halfQ) mod p

    // Semantic check: running the executor's 4-op sequence (REDC multiply +
    // plain add, immediates as computed) on Montgomery-encoded input must
    // produce the Montgomery encoding of the centered rebase that the
    // ordinary 3-op lowering computes.
    for (uint64_t v : {uint64_t{0}, uint64_t{1}, half_q, half_q + 1, q - 1}) {
        // Ordinary reference: center mod q (signed-preserving), reduce mod p.
        const uint64_t shifted = (v + half_q) % q;
        const uint64_t rebased = shifted % p;
        const uint64_t expected = (rebased + (p - (half_q % p))) % p;

        uint64_t x = to_montgomery(v, q);
        x = ref_montgomery_mul(x, mont.imm[0], q); // REDC out of Montgomery form
        x = (x + mont.imm[1]) % q;                 // centered shift, ordinary form
        x = ref_montgomery_mul(x, mont.imm[2], p); // rebase + back into Montgomery
        x = (x + mont.imm[3]) % p;                 // Montgomery-form un-shift
        REQUIRE(x == to_montgomery(expected, p));
    }
}

TEST_CASE("data format config: setter beats env var beats default", "[unit][hwfmt]") {
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Default: both off.
    REQUIRE_FALSE(haze::config().montgomery());
    REQUIRE_FALSE(haze::config().bit_reversal());

    // Env enables independently.
    ::setenv("HAZE_MONTGOMERY", "1", 1); // NOLINT(misc-include-cleaner)
    REQUIRE(haze::config().montgomery());
    REQUIRE_FALSE(haze::config().bit_reversal());
    ::setenv("HAZE_BIT_REVERSAL", "true", 1); // NOLINT(misc-include-cleaner)
    REQUIRE(haze::config().bit_reversal());

    // Explicit setter beats env.
    REQUIRE(hazeSetMontgomery(0) == HAZE_SUCCESS);
    REQUIRE_FALSE(haze::config().montgomery());
    REQUIRE(haze::config().bit_reversal()); // untouched

    // Reset clears the explicit setter, so the env applies again.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(haze::config().montgomery());

    // Both flags set independently via explicit setters.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);
    REQUIRE(haze::config().montgomery());
    REQUIRE(haze::config().bit_reversal());

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("data format on local target is rejected at flush", "[unit][hwfmt]") {
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // make test-unit exports HAZE_TARGET=local; pin it programmatically so
    // the case also holds when run standalone.
    REQUIRE(hazeSetTarget("local") == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);

    REQUIRE(hazeFlush() == HAZE_ERROR_NOT_SUPPORTED);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_NOT_SUPPORTED);

    // Turning the format off restores normal local operation.
    REQUIRE(hazeSetMontgomery(0) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(0) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("data format rejected at flush keeps the in-flight recording", "[integration][hwfmt]") {
    // The flush guard early-returns before replay_and_populate, leaving the
    // epoch open. After the config is corrected the same recording must still
    // materialize — the op is not silently dropped. Pinned local so the
    // rejection fires regardless of the harness target.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, picked) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget("local") == HAZE_SUCCESS);

    std::vector<uint64_t> vals(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        vals[i] = (i * 3 + 1) % picked;
    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(dst, src, 3, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);

    // Enable the data format and flush: rejected on the local target.
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_ERROR_NOT_SUPPORTED);

    // Correct the config; the same recorded op now materializes.
    REQUIRE(hazeSetMontgomery(0) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(0) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k);
        REQUIRE(out[k] == niobium::mod_arith::mulmod(vals[k], 3, picked));
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("record-time: recording stays ordinary-form with switchmodulus markers",
          "[unit][hwfmt]") {
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget("FUNC_SIM") == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);

    const auto dir = record_program("haze_hwfmt_on", /*with_mod_down=*/true);

    // Client traces are ordinary-form even with the data format on:
    // encoding (input data, immediates, switchmodulus substitution)
    // happens driver-side, selected by the --niobium_hw flag the dispatch
    // appends. The scalar immediate must appear verbatim, not encoded.
    const std::string trace = slurp(dir / "haze_hwfmt_on.fhetch");
    const uint64_t mont_imm = niobium::mod_arith::to_montgomery(uint64_t{7}, kQ0);
    REQUIRE(trace.find(", 7,") != std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(mont_imm) + ",") == std::string::npos);

    // The mod-down's centered rebase emits the bare canonical 4-op block
    // with ordinary immediates — no marker comment. The replay driver
    // recognizes the quadruple structurally (the immediates are fully
    // determined by (q, p)) and substitutes the SwitchModulus pseudo-op
    // with format-correct immediates at replay.
    REQUIRE(trace.find("# switchmodulus") == std::string::npos);
    const auto sw = niobium::mod_arith::compute_switchmodulus_immediates(kQ1, kQ0,
                                                                         /*montgomery=*/false);
    REQUIRE(trace.find(", " + std::to_string(sw.imm[1]) + ",") != std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(sw.imm[3]) + ",") != std::string::npos);
    // The Montgomery-form immediates the driver substitutes must NOT be in
    // the recorded trace.
    const auto sw_hw = niobium::mod_arith::compute_switchmodulus_immediates(kQ1, kQ0,
                                                                            /*montgomery=*/true);
    REQUIRE(trace.find(", " + std::to_string(sw_hw.imm[2]) + ",") == std::string::npos);

    // Replay JSON: niobium_hw describes the recording (ordinary-form), so it
    // stays false regardless of the toggles; the dispatch flag carries the
    // replay-side format selection.
    const std::string replay_json = slurp(dir / "fhetch_replay.json");
    REQUIRE(json_value_text(replay_json, "niobium_hw") == "false");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// Captured input values must stay raw (ordinary-form, natural order) even
// under --niobium_hw: the replay driver converts input data itself (keyed on
// the project's niobium_hw=false), so a client-side conversion would
// double-encode. Drive the public Compiler API directly and inspect the
// record before haze clears it.
TEST_CASE("store_input_element keeps values ordinary under the data format", "[unit][hwfmt]") {
    auto &cc = niobium::compiler();
    cc.reset();
    {
        std::string prog = "hwfmt_store_input";
        std::string flag = "--niobium_hw";
        char *argv[] = {prog.data(), flag.data(), nullptr};
        int argc = 2;
        cc.init(argc, argv);
    }

    std::vector<uint64_t> raw(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        raw[i] = ((i * 31) + 5) % kQ0;
    cc.store_input_element("hwfmt_in", niobium::CapturedKind::SRP,
                           /*starts_new_element=*/false, /*addr_id=*/1, kQ0, raw);

    bool found_raw_input = false;
    niobium::detail::for_each_captured_input([&](const niobium::CapturedInputRecord &rec) {
        for (const auto &residue : rec.per_residue_values) {
            if (residue == raw)
                found_raw_input = true;
        }
    });
    REQUIRE(found_raw_input);

    // This case drove niobium::compiler() directly with --niobium_hw;
    // hazeDeviceReset only clears CompilerBackend's initialized_ flag, so
    // reset the compiler too or the next ensure_initialized() re-init's an
    // already-running, --niobium_hw instance.
    cc.reset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("record-time: modes off leaves trace and JSON in ordinary form", "[unit][hwfmt]") {
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetTarget("FUNC_SIM") == HAZE_SUCCESS);

    const auto dir = record_program("haze_hwfmt_off", /*with_mod_down=*/false);

    // The scalar immediate appears verbatim and its Montgomery encoding
    // does not — identical to the toggles-on case, since recordings are
    // always ordinary-form.
    const std::string trace = slurp(dir / "haze_hwfmt_off.fhetch");
    const uint64_t mont_imm = niobium::mod_arith::to_montgomery(uint64_t{7}, kQ0);
    REQUIRE(trace.find(", 7,") != std::string::npos);
    REQUIRE(trace.find(", " + std::to_string(mont_imm) + ",") == std::string::npos);
    const std::string replay_json = slurp(dir / "fhetch_replay.json");
    REQUIRE(json_value_text(replay_json, "niobium_hw") == "false");

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ===========================================================================
// Transport e2e — these record + replay through nbcc_fhetch_replay, so they
// runtime-skip under the local in-process simulator (which rejects the
// data format by design). They run under `make test-transport`.
//
// Byte-exactness invariant: Montgomery + bit-reversal are exact bijections
// the replay engine decodes before writing probes, so data-format
// results must be byte-identical to ordinary-form results. Every assertion
// below is exact equality — any mismatch is a bug, not tolerance.
// ===========================================================================

namespace {

bool transport_target_active() {
    const char *target = std::getenv("HAZE_TARGET");
    return target != nullptr && target[0] != '\0' && std::string_view{target} != "local";
}

// Record a fixed mixed computation (elementwise ops + mod-down) with the
// current format settings under a unique program name, flush through the
// transport, and return the D2H output vectors (sum, prod, scaled, down).
std::vector<std::vector<uint64_t>> run_mixed_computation(bool with_mod_down) {
    // Use the default shared "haze" program dir: the replay bridge plants
    // program_name "haze" at init and writes cryptocontext.dat there, so a
    // per-test rename would strand the crypto context the transport driver
    // needs for probe serialization. hazeDeviceReset between cases keeps the
    // shared dir clean (same pattern as the rest of the integration suite).
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(1, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ2) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 101, kRingDim);
    const std::vector<uint64_t> b = haze::test::make_residue(kQ0, 202, kRingDim);
    const std::vector<uint64_t> r1 = haze::test::make_residue(kQ1, 303, kRingDim);

    void *pa = nullptr;
    void *pb = nullptr;
    void *p1 = nullptr;
    void *sum = nullptr;
    void *prod = nullptr;
    void *scaled = nullptr;
    void *down = nullptr;
    for (void **p : {&pa, &pb, &p1, &sum, &prod, &scaled, &down})
        REQUIRE(hazeMalloc(p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pa, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pb, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(p1, r1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(sum, pa, pb, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMul(prod, pa, pb, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(scaled, pa, 7, 0, nullptr) == HAZE_SUCCESS);

    // Mod-down exercises the FBC gadget (and, under --niobium_hw, the
    // driver's switchmodulus-block substitution).
    if (with_mod_down) {
        const uint64_t src_base[] = {kQ0, kQ1};
        const uint64_t rescale_base[] = {kQ1};
        const void *src_polys[] = {pa, p1};
        void *dst_polys[] = {down};
        hazeModDownParams p{};
        p.src_base = src_base;
        p.src_base_len = 2;
        p.rescale_base = rescale_base;
        p.rescale_base_len = 1;
        REQUIRE(hazeModDown(dst_polys, src_polys, &p, nullptr) == HAZE_SUCCESS);
    } else {
        // Keep the output list shape: alias `down` to a plain copy.
        REQUIRE(hazeMemcpy(down, pa, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) == HAZE_SUCCESS);
    }

    // NOTE: basis-convert with overlapping primes (the copy_residue
    // pass-through) is exercised by the existing ordinary-mode suite and
    // currently fails on the FUNC_SIM transport even without the data
    // format (pre-existing; see "hazeBasisConvert: shared-modulus copies
    // produce input values" under make test-transport), so it is excluded
    // here until that path is fixed compiler-side.
    for (void *out : {sum, prod, scaled, down})
        REQUIRE(hazeTagOutput(out) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<std::vector<uint64_t>> results;
    for (void *out : {sum, prod, scaled, down}) {
        std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(got.data(), out, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        results.push_back(std::move(got));
    }
    return results;
}

} // namespace

TEST_CASE("data-format transport: elementwise results byte-exact vs plain oracle",
          "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);

    const auto results = run_mixed_computation(/*with_mod_down=*/false);

    // Plain (ordinary-form) oracles — the replay engine decodes the
    // Montgomery/bit-reversed outputs back, so the data format must be
    // invisible in the results.
    const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 101, kRingDim);
    const std::vector<uint64_t> b = haze::test::make_residue(kQ0, 202, kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k);
        const uint64_t s = a[k] + b[k];
        REQUIRE(results[0][k] == (s >= kQ0 ? s - kQ0 : s));
        REQUIRE(results[1][k] == niobium::mod_arith::mulmod(a[k], b[k], kQ0));
        REQUIRE(results[2][k] == niobium::mod_arith::mulmod(a[k], 7, kQ0));
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("data-format transport: A/B byte-exact vs ordinary mode", "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});

    // Same recorded computation, replayed once in ordinary form and once in
    // data format. The Montgomery/bit-reversal encoding is a temporary
    // internal representation, so the full output vectors (including the
    // FBC/mod-down path) must compare byte-for-byte equal.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto ordinary = run_mixed_computation(/*with_mod_down=*/true);

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);
    const auto encoded = run_mixed_computation(/*with_mod_down=*/true);

    REQUIRE(ordinary.size() == encoded.size());
    for (size_t i = 0; i < ordinary.size(); ++i) {
        INFO("output " << i);
        REQUIRE(ordinary[i] == encoded[i]);
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("data-format transport: NTT round trip and automorph under data format",
          "[integration][hwfmt]") {
    if (!transport_target_active())
        SKIP("data format requires a transport target (run under make test-transport)");
    EnvGuard guard({"HAZE_MONTGOMERY", "HAZE_BIT_REVERSAL"});

    // In-contract NTT/morph use under the data format, A/B byte-exact
    // against ordinary mode.
    //
    // By contract, program inputs are always
    // evaluation-form, and the storage layout (bit-reversed order) plus the
    // hw NTT's I/O conventions are self-consistent only for data that stays
    // inside a trace. A STANDALONE forward NTT of a program input is
    // out-of-contract: the input is conceptually coefficient-form, the load
    // path bit-reverses it like all inputs, and an NTT does not commute with
    // permuting its input — verified bit-for-bit that encoded_NTT(x) ==
    // ordinary_NTT(bitrev(x)) on every slot. So this test exercises the
    // in-contract INTT -> NTT round trip (the coefficient intermediate never
    // crosses the program boundary), and the MRP automorph (the
    // modulus-carrying path; the bare SRP hazeAutomorph is modulus-less, so
    // its output template keeps the bridge's synthetic prime and cannot be
    // probe-filled under the data format).
    auto run_ntt = [&]() {
        REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
        uint64_t picked = 0;
        REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
        REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
        REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

        const std::vector<uint64_t> a = haze::test::make_residue(kQ0, 404, kRingDim);
        void *src = nullptr;
        void *coeff = nullptr;
        void *fwd = nullptr;
        void *rot = nullptr;
        for (void **p : {&src, &coeff, &fwd, &rot})
            REQUIRE(hazeMalloc(p, kBytes) == HAZE_SUCCESS);
        REQUIRE(hazeMemcpy(src, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
        // In-contract NTT use: program inputs are always evaluation-form on
        // the executor; coefficient-form data exists only BETWEEN
        // executor ops. Exercise the INTT -> NTT round trip so the
        // coefficient intermediate never crosses the program boundary.
        REQUIRE(hazeINTT(coeff, src, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeNTT(fwd, coeff, 0, nullptr) == HAZE_SUCCESS);
        const uint64_t base[] = {kQ0};
        void *rot_dst[] = {rot};
        const void *rot_src[] = {src};
        REQUIRE(hazeAutomorphMrp(rot_dst, rot_src, 5, base, 1, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(fwd) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(rot) == HAZE_SUCCESS);
        REQUIRE(hazeFlush() == HAZE_SUCCESS);

        std::vector<void *> outs = {fwd, rot};
        std::vector<std::vector<uint64_t>> results;
        for (void *out : outs) {
            std::vector<uint64_t> got(kRingDim, 0xDEADBEEFULL);
            REQUIRE(hazeMemcpy(got.data(), out, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
                    HAZE_SUCCESS);
            results.push_back(std::move(got));
        }
        return results;
    };

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const auto ordinary = run_ntt();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);
    const auto encoded = run_ntt();
    for (size_t i = 0; i < ordinary.size(); ++i) {
        INFO("output " << i);
        REQUIRE(ordinary[i] == encoded[i]);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// The trace's modulus table — not the bridge's scaffold cryptocontext.dat
// prime — is authoritative for replay. Three distinct valid primes are in
// play: kQ0 (the desired modulus requested of the bridge), `picked` (what
// OpenFHE's GenCryptoContext actually built cryptocontext.dat around), and the
// trace modulus we set (kQ1). The add is crafted to WRAP — a+b lands in
// [picked, kQ1) — so reducing mod kQ1 (no wrap) and mod picked (wrap) give
// different results. The result must equal the kQ1 oracle, proving the
// install-trace machinery makes the trace authoritative on this target.
TEST_CASE("trace modulus is authoritative over the bridge's scaffold prime",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(picked != kQ0);
    REQUIRE(picked != kQ1);
    // picked < kQ1 holds for these constants; the wrap window [picked, kQ1)
    // below relies on it.
    REQUIRE(picked < kQ1);
    REQUIRE(hazeSetCiphertextModulus(0, kQ1) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    // a = kQ1 - 7, b = 11  =>  a + b = kQ1 + 4.
    //   mod kQ1    = 4              (trace authoritative)
    //   mod picked = (kQ1-picked)+4 (scaffold prime authoritative)
    // a is a valid residue mod kQ1 but exceeds picked, so the two paths can't
    // coincide.
    std::vector<uint64_t> va(kRingDim, kQ1 - 7);
    std::vector<uint64_t> vb(kRingDim, 11);
    const uint64_t expect_kq1 = 4;
    const uint64_t expect_picked = (kQ1 - picked) + 4;
    REQUIRE(expect_kq1 != expect_picked);

    void *pa = nullptr;
    void *pb = nullptr;
    void *sum = nullptr;
    REQUIRE(hazeMalloc(&pa, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&pb, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&sum, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pa, va.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(pb, vb.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(sum, pa, pb, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(sum) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), sum, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k << " picked=" << picked);
        REQUIRE(out[k] == expect_kq1);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// Input residues in [scaffold_prime, trace_prime) must survive serialization.
// The bridge builds the input ciphertext from OpenFHE's scaffold prime, which
// is SMALLER than the trace prime here (measured picked < kQ0), so a residue
// in that window would wrap if filled before the modulus was rebased to the
// trace prime. synthesize rebases each tower first, so the .bin carries the
// exact trace prime and the value round-trips. Random residues land in this
// ~3.4M-wide window with negligible probability, so this pins it explicitly.
TEST_CASE("input residues above the scaffold prime survive .bin synthesis",
          "[integration][modulus]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(picked < kQ0); // the wrap window [picked, kQ0) below relies on it
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    // Seed the wrap window: values that exceed the scaffold prime but are
    // valid mod the trace prime. A scalar-mul by 1 is the identity, so each
    // output must equal its input exactly.
    std::vector<uint64_t> vals(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        vals[i] = (i % 5 == 0) ? (kQ0 - 1 - (i % 7)) // in [picked, kQ0)
                               : (i * 11 + 3) % picked;
    vals[1] = picked;     // exact lower edge of the window
    vals[2] = picked - 1; // just below — control

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src, vals.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMulScalar(dst, src, 1, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (uint64_t k = 0; k < kRingDim; ++k) {
        INFO("slot " << k << " in=" << vals[k] << " picked=" << picked << " kQ0=" << kQ0);
        REQUIRE(out[k] == vals[k]);
    }
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
