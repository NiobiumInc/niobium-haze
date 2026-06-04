// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// FIDESlib-coexistence proof.
//
// Goal: demonstrate that the shipped, symbol-isolated libhaze.so — which
// statically absorbs the niobium OpenFHE 1.4.2 fork and exports ONLY the haze*
// C ABI — can run in the SAME process as a second, ABI-incompatible OpenFHE
// (here OpenFHE 1.5.1, the release FIDESlib pins, linked statically into THIS
// executable) without any symbol collision.
//
// This TU sees ONLY:
//   * haze's C ABI (haze/haze.h, haze/replay_bridge.h — both OpenFHE-free), and
//   * OpenFHE 1.5.1 headers (for the FIDESlib-side crypto).
// It must NOT include any niobium-1.4.2 OpenFHE header (that lives hidden
// inside libhaze.so) nor replay_bridge_cc.hpp (which would pull 1.4.2 lbcrypto
// types). Only uint64_t limb arrays and scalars cross the haze boundary.
//
// The two FHE computations are kept INDEPENDENT (no limbs fed from one stack
// into the other), so the implicit numeric-convention differences between
// 1.4.2 and 1.5.1 (prime selection, NTT/twiddle layout) cannot affect the
// result — this proves linkage isolation, not cross-stack interchange.

#include <haze/haze.h>            // haze C ABI (no OpenFHE types)
#include <haze/replay_bridge.h>  // pure-C bridge control surface (no OpenFHE types)

#include "openfhe.h" // OpenFHE 1.5.1 (FIDESlib's), linked statically into this exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (ok) {
        std::printf("  PASS: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// --- FIDESlib side: a real OpenFHE 1.5.1 CKKS encrypt / EvalAdd / decrypt. ---
// Returns true iff the decrypted slots match the cleartext sum within tol.
bool run_openfhe_1_5_1_ckks_add() {
    using namespace lbcrypto;

    CCParams<CryptoContextCKKSRNS> parameters;
    parameters.SetMultiplicativeDepth(1);
    parameters.SetScalingModSize(50);
    parameters.SetBatchSize(8);

    CryptoContext<DCRTPoly> cc = GenCryptoContext(parameters);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    auto keys = cc->KeyGen();

    const std::vector<double> x1 = {1.0, 2.0, 3.0, 4.0};
    const std::vector<double> x2 = {5.0, 6.0, 7.0, 8.0};

    Plaintext p1 = cc->MakeCKKSPackedPlaintext(x1);
    Plaintext p2 = cc->MakeCKKSPackedPlaintext(x2);

    auto c1 = cc->Encrypt(keys.publicKey, p1);
    auto c2 = cc->Encrypt(keys.publicKey, p2);
    auto cadd = cc->EvalAdd(c1, c2);

    Plaintext result;
    cc->Decrypt(keys.secretKey, cadd, &result);
    result->SetLength(static_cast<int>(x1.size()));

    const auto got = result->GetCKKSPackedValue(); // std::vector<std::complex<double>>
    bool ok = got.size() >= x1.size();
    for (std::size_t i = 0; ok && i < x1.size(); ++i) {
        const double expected = x1[i] + x2[i];
        if (std::abs(got[i].real() - expected) > 1e-2) {
            ok = false;
        }
    }
    return ok;
}

// --- haze side: drive the C ABI, which runs the HIDDEN 1.4.2 OpenFHE/fhetch
// in-process simulator. hazeReplayBridgeInitCryptoContext forces libhaze's
// internal 1.4.2 OpenFHE to build a CryptoContext — the strongest exercise of
// the absorbed-and-hidden stack while 1.5.1 is also live in this process.
bool run_haze_polynomial_add() {
    constexpr uint64_t kN = 4096;
    constexpr uint64_t kBytes = kN * sizeof(uint64_t);
    constexpr uint64_t kDesiredModulus = 576460752303415297ULL; // q ≡ 1 (mod 2N)

    if (hazeDeviceReset() != HAZE_SUCCESS) return false;
    if (hazeSetRingDimension(kN) != HAZE_SUCCESS) return false;

    uint64_t picked = 0;
    if (hazeReplayBridgeInitCryptoContext(kN, kDesiredModulus, &picked) != HAZE_SUCCESS) return false;
    if (picked == 0) return false; // libhaze's hidden 1.4.2 OpenFHE picked a prime

    if (hazeSetCiphertextModulus(0, picked) != HAZE_SUCCESS) return false;
    if (hazeConfigureDevice() != HAZE_SUCCESS) return false;

    // Allocate all three up front; a single null-guarded cleanup at the end
    // frees whatever was allocated on every path (no leak if a later step
    // fails). Short-circuit && stops at the first failed malloc.
    void *da = nullptr;
    void *db = nullptr;
    void *dc = nullptr;
    bool ok = false;
    if (hazeMalloc(&da, kBytes) == HAZE_SUCCESS &&
        hazeMalloc(&db, kBytes) == HAZE_SUCCESS &&
        hazeMalloc(&dc, kBytes) == HAZE_SUCCESS) {
        std::vector<uint64_t> ha(kN);
        std::vector<uint64_t> hb(kN);
        for (uint64_t i = 0; i < kN; ++i) {
            ha[i] = (i * 1315423911ULL + 7ULL) % picked;
            hb[i] = (i * 2654435761ULL + 13ULL) % picked;
        }

        std::vector<uint64_t> hc(kN, 0xDEADBEEFULL);
        if (hazeMemcpy(da, ha.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS &&
            hazeMemcpy(db, hb.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS &&
            hazeAdd(dc, da, db, /*mod_idx=*/0, /*stream=*/nullptr) == HAZE_SUCCESS &&
            hazeMemcpy(hc.data(), dc, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS) {
            ok = true;
            for (uint64_t i = 0; ok && i < kN; ++i) {
                const uint64_t expected = (ha[i] + hb[i]) % picked;
                if (hc[i] != expected) ok = false;
            }
        }
    }

    if (da) hazeFree(da);
    if (db) hazeFree(db);
    if (dc) hazeFree(dc);
    return ok;
}

} // namespace

int main() {
    std::printf("[coexistence] OpenFHE 1.5.1 (this exe) + hidden 1.4.2 (libhaze.so)\n");

    // 1.5.1 works on its own.
    check(run_openfhe_1_5_1_ckks_add(), "OpenFHE 1.5.1 CKKS encrypt/EvalAdd/decrypt");

    // haze drives its hidden 1.4.2 OpenFHE while 1.5.1 state is already live.
    check(run_haze_polynomial_add(), "haze C-ABI add via hidden 1.4.2 simulator");

    // 1.5.1 still works after haze ran — neither stack corrupted the other's
    // process-global OpenFHE state (CryptoContext/key/serialization registries).
    check(run_openfhe_1_5_1_ckks_add(), "OpenFHE 1.5.1 again, post-haze (no cross-corruption)");

    if (g_failures == 0) {
        std::printf("[coexistence] ALL CHECKS PASSED\n");
        return EXIT_SUCCESS;
    }
    std::printf("[coexistence] %d CHECK(S) FAILED\n", g_failures);
    return EXIT_FAILURE;
}
