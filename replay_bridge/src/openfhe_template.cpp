// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge implementation: synthesize the CryptoContext +
// per-input cereal-binary .bin files + per-output ciphertext templates that
// the in-process FHETCH simulator AND compiler-side nbcc_fhetch_replay both
// consume to write serialized_probes/<name>.ct from a haze recording.
// See replay_bridge.h for the public API and architectural rationale.
//
// Thread-safety: the bridge is called only from haze's epoch path, which
// already holds EpochState::mutex_, and from haze tests (single-threaded
// catch2 runner). Bridge state is therefore implicitly serialized; no
// internal lock needed.

#include <haze/replay_bridge.h>

#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include "common/log.hpp"

#include <niobium/compiler.h>

#include <bit>
#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// libnbfhetch internal helpers used by the bridge. Forward-declared here
// rather than via "compiler_internal.h" because that header lives under
// libnbfhetch's PRIVATE src/ tree and including it from a downstream
// library would couple the bridge to libnbfhetch's source layout.
//
// TODO(fhetch-publish): the four detail symbols below resolve in
// libnbfhetch's TU because cereal polymorphic-type registrations are
// per-shared-library — calling lbcrypto::Serial::SerializeToFile from
// here directly would trip "unregistered polymorphic type" on
// CryptoContextImpl / Ciphertext. Long-term, ask niobium-fhetch to
// publish a stable C++ API for write_ciphertext_template,
// write_ciphertext_input_bin, for_each_captured_input, and
// for_each_captured_output, then replace the externs with that header
// include. niobium-client's auto-facade routes through the same
// libnbfhetch TU (auto_facade.cpp's serialize helpers) so a published
// API would also let niobium-client drop its own forwarder shims.
namespace niobium::detail {

// Serialize an empty Ciphertext<DCRTPoly> shell to
// <program_dir>/ciphertext_templates/<name>.template. The replay path
// (in-process simulator or transport) deserializes this template,
// fills its first NativePoly with computed values, and re-serializes
// it as serialized_probes/<name>.ct.
bool write_ciphertext_template(
    const std::string& name,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct);

// Serialize a populated Ciphertext<DCRTPoly> as the cereal-binary
// {.bin, .ids} pair the compiler-side fhetch_driver expects for an
// input named `name`.
bool write_ciphertext_input_bin(
    const std::string& name,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
    const std::vector<uint64_t>& addr_ids);

// Iterate every captured input recorded via fhetch::tag_input. The
// callback receives (name, addr_id, modulus, values) for each per-name
// element so the bridge can fan-out a single ciphertext per name.
void for_each_captured_input(
    const std::function<void(const std::string&, uint64_t, uint64_t,
                              const std::vector<uint64_t>&)>& cb);

// Iterate every captured output name recorded via fhetch::tag_output.
// Callback fires once per output name.
void for_each_captured_output(
    const std::function<void(const std::string&)>& cb);

}  // namespace niobium::detail

namespace {

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// BridgeError surfaces specific failure modes from the C-ABI entry to
// help callers diagnose synthesis issues. Lives in this file (rather
// than a header) because there's exactly one caller and one
// implementation TU.
enum class BridgeError {
    InvalidRingDim,
    InvalidModulus,
    KeygenFailed,
    OpenFheUnavailable,
};

// Per-process cache: same (ring_dim, desired_modulus) reuses the same
// CryptoContext + keypair across cryptocontext.dat, every template, and
// every input bin. OpenFHE resolves Ciphertext<->CryptoContext bindings
// through a static tag registry on deserialize; sharing the CC keeps
// the tag stable.
//
// The keypair stays cached even though we don't decrypt — Encrypt()
// needs a publicKey to produce a structurally valid Ciphertext<DCRTPoly>.
// See TODO(haze:no-keygen) below.
struct CachedContext {
    uint64_t ring_dim = 0;
    uint64_t desired_modulus = 0;
    uint64_t picked_modulus = 0;
    lbcrypto::CryptoContext<DCRTPoly> cc;
    lbcrypto::KeyPair<DCRTPoly> keys;
};

CachedContext g_ctx;

// Number of bits needed to represent a CKKS prime modulus. Modulus is
// asserted >= 2 because OpenFHE's GenCryptoContext rejects smaller
// values upstream and the bridge's only caller path has already
// validated the input. std::bit_width handles 2^63 cleanly (returns 64)
// where an iterative shift would silently truncate.
uint32_t bit_width_for_modulus(uint64_t modulus) {
    assert(modulus >= 2);
    return static_cast<uint32_t>(std::bit_width(modulus));
}

// Build (or rebuild) the cached CryptoContext + keypair to match
// (ring_dim, desired_modulus). Returns success on cache hit (already
// correct shape) or after a successful rebuild; returns a structured
// BridgeError otherwise so the caller can map to the matching
// hazeError_t.
std::expected<void, BridgeError>
rebuild_context_locked(uint64_t ring_dim, uint64_t desired_modulus) {
    using namespace lbcrypto;
    // Sanity gate: 0 or 1 are trivially malformed. Real CKKS NTT-
    // friendly primes are ~60-bit; OpenFHE will reject other invalid
    // choices deeper in GenCryptoContext.
    if (ring_dim == 0)
        return std::unexpected(BridgeError::InvalidRingDim);
    if (desired_modulus < 2)
        return std::unexpected(BridgeError::InvalidModulus);

    if (g_ctx.cc && g_ctx.ring_dim == ring_dim &&
        g_ctx.desired_modulus == desired_modulus) {
        return {};  // cache hit
    }

    CCParams<CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(HEStd_NotSet);  // toy: ring_dim is user-chosen
    p.SetRingDim(ring_dim);
    // TODO(haze:multi-residue): SetMultiplicativeDepth(0) produces a
    // single-tower DCRTPoly chain. Multi-residue paths (hazeModUp,
    // hazeModDown) require depth > 0 and a fan of NativePolys per
    // ciphertext element — see the failing integration tests in
    // test/test_basis_convert.cpp at lines 117/245/291/331/381/443/757.
    p.SetMultiplicativeDepth(0);
    const uint32_t bits = bit_width_for_modulus(desired_modulus);
    p.SetScalingModSize(bits - 1);
    p.SetFirstModSize(bits);
    // FIXEDMANUAL leaves rescale to the caller. At depth 0 with a
    // single tower, OpenFHE's auto-rescale modes would inject extra
    // modulus elements between operations and break the 1-NativePoly
    // invariant the bridge depends on (synthesize_haze_ciphertext_locked
    // assumes ct->GetElements()[0].GetAllElements().size() == 1).
    p.SetScalingTechnique(FIXEDMANUAL);

    auto cc = GenCryptoContext(p);
    if (!cc)
        return std::unexpected(BridgeError::OpenFheUnavailable);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    // TODO(haze:no-keygen): we keygen + Encrypt + trim only because
    // OpenFHE doesn't expose a public no-key Ciphertext<DCRTPoly>
    // constructor. The bridge never decrypts; the keygen is effectively
    // structural ballast. If a public empty-ciphertext factory becomes
    // available in OpenFHE (e.g. cc->NewCiphertext()), drop the keypair
    // and the Encrypt round-trip in synthesize_haze_ciphertext_locked.
    auto keys = cc->KeyGen();
    if (!keys.publicKey || !keys.secretKey)
        return std::unexpected(BridgeError::KeygenFailed);

    auto element_params = cc->GetCryptoParameters()->GetElementParams();
    if (!element_params || element_params->GetParams().empty())
        return std::unexpected(BridgeError::OpenFheUnavailable);
    uint64_t picked = element_params->GetParams()[0]->GetModulus().ConvertToInt();

    g_ctx.ring_dim = ring_dim;
    g_ctx.desired_modulus = desired_modulus;
    g_ctx.picked_modulus = picked;
    g_ctx.cc = cc;
    g_ctx.keys = keys;
    return {};
}

// Map a BridgeError to the matching hazeError_t for the C ABI. The
// granular BridgeError values exist for diagnostics (log_error tags,
// future debug surfaces); the public hazeError_t is intentionally
// coarser.
hazeError_t to_haze_error(BridgeError e) {
    switch (e) {
        case BridgeError::InvalidRingDim:
        case BridgeError::InvalidModulus:
            return HAZE_ERROR_INVALID_VALUE;
        case BridgeError::KeygenFailed:
        case BridgeError::OpenFheUnavailable:
            return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_LAUNCH_FAILURE;
}

// Build a 1-element / 1-tower Ciphertext<DCRTPoly> and fill its first
// NativePoly with `values`. Returns the synthesized ciphertext.
//
// TODO(haze:no-keygen): bypass Encrypt + element[1] trim with a direct
// public-API no-key construction once OpenFHE exposes one. See the
// rebuild_context_locked TODO.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_haze_ciphertext(const std::vector<uint64_t>& values) {
    std::vector<double> zeros(g_ctx.ring_dim / 2, 0.0);
    auto pt = g_ctx.cc->MakeCKKSPackedPlaintext(zeros);
    auto ct = g_ctx.cc->Encrypt(g_ctx.keys.publicKey, pt);
    // Trim the RLWE pair (a, b) down to a 1-element ciphertext so the
    // .bin's NativePoly count matches haze's one-polynomial-per-input
    // model. element[1] would otherwise land in the .bin as Encrypt-noise
    // and the compiler-side would auto-allocate a colliding addr_id for
    // it. The result is not-decryptable, but we never decrypt — the
    // compiler-side reads NativePoly values positionally, not via the
    // CKKS API.
    {
        auto& els = ct->GetElements();
        if (els.size() > 1) els.resize(1);
    }
    if (values.empty()) {
        // Output template path: caller wants an empty shell, no fill.
        return ct;
    }
    auto& dcrt = ct->GetElements()[0];
    auto& np = dcrt.GetAllElements()[0];
    size_t n = np.GetParams()->GetRingDimension();
    if (values.size() != n) {
        std::ostringstream body;
        body << "values.size()=" << values.size() << " != ring_dim=" << n
             << " — cannot fill input ciphertext";
        haze::log_error("replay_bridge", body.str());
        return ct;
    }
    lbcrypto::NativeVector nv(n, lbcrypto::NativeInteger(np.GetModulus()));
    for (size_t i = 0; i < n; ++i)
        nv[i] = lbcrypto::NativeInteger(values[i]);
    np.SetValues(nv, np.GetFormat());
    return ct;
}

// LOAD-BEARING. Synthesizes per-program OpenFHE artifacts the replay
// path (in-process simulator or transport-side nbcc_fhetch_replay)
// needs to consume haze's pure-FHETCH recording. Must run AFTER
// trace_writer.stop_recording() — so for_each_captured_* sees the
// final input/output sets — and BEFORE the replay path's reconstruct
// step (so cereal type registrations resolve in libnbfhetch's TU).
// Wired up via niobium::compiler().set_post_recording_hook(...).
void on_post_recording() {
    if (!g_ctx.cc) {
        // No CC initialized for this session — bridge was never called,
        // so there are no haze-style artifacts to materialize.
        return;
    }

    // ---- Inputs ----
    struct InputAccum {
        std::vector<uint64_t> values;  // first element's values
        std::vector<uint64_t> addr_ids;
        bool seen_first = false;
    };
    std::map<std::string, InputAccum> inputs_by_name;

    niobium::detail::for_each_captured_input(
        [&](const std::string& name, uint64_t addr_id, uint64_t /*mod*/,
            const std::vector<uint64_t>& values) {
            if (name == "evalmult_key" || name == "automorphism_key") return;
            auto& acc = inputs_by_name[name];
            acc.addr_ids.push_back(addr_id);
            if (!acc.seen_first) {
                acc.values = values;
                acc.seen_first = true;
            }
        });

    for (auto& [name, acc] : inputs_by_name) {
        try {
            auto ct = synthesize_haze_ciphertext(acc.values);
            if (!niobium::detail::write_ciphertext_input_bin(
                    name, ct, acc.addr_ids)) {
                haze::log_error(
                    "replay_bridge",
                    "write_ciphertext_input_bin failed for '" + name + "'");
            }
        } catch (const std::exception& e) {
            haze::log_error(
                "replay_bridge",
                "input bin synthesis for '" + name + "' threw: " + e.what());
        }
    }

    // ---- Outputs ----
    // Auto-write a 1-element template for each named output. The replay
    // path fills the first NativePoly with simulator output; templates
    // are otherwise empty. This removes the burden from test code of
    // having to call hazeReplayBridgeWriteTemplate per output — tests
    // just call hazeReplay() and the bridge handles it.
    niobium::detail::for_each_captured_output(
        [&](const std::string& name) {
            try {
                auto ct = synthesize_haze_ciphertext({});
                if (!niobium::detail::write_ciphertext_template(name, ct)) {
                    haze::log_error(
                        "replay_bridge",
                        "write_ciphertext_template failed for '" + name + "'");
                }
            } catch (const std::exception& e) {
                haze::log_error(
                    "replay_bridge",
                    "template synthesis for '" + name + "' threw: " + e.what());
            }
        });
}

// Push the bridge's CryptoContext into niobium::compiler() (libnbfhetch's
// "dummy" compiler — NOT the niobium-compiler nbcc) via the existing
// OpenFHE-aware capture path. capture_crypto_context lives inside
// libnbfhetch (auto_facade.cpp), so the cereal polymorphic
// registrations for CryptoContextImpl<DCRTPoly> resolve in the same
// shared library where the cc.dat serialization runs.
//
// capture_crypto_context internally:
//   - calls set_ring_dimension(N)
//   - calls set_crypto_context_info(scheme, depth, ..., modulus_chain)
//   - serializes the CC to <program_dir>/cryptocontext.dat
//   - registers an auto_capture_at_stop hook that calls
//     tag_bootstrap_precompute on the CC
//
// We want the first three but NOT the fourth — haze records pure
// polynomial-level ops, no bootstrap precompute. Clear the hook
// immediately after capture so stop() doesn't fire it. Set our own
// post_recording_hook so on_post_recording fires on stop().
void push_crypto_to_compiler() {
    niobium::compiler().capture_crypto_context(g_ctx.cc);
    niobium::compiler().set_auto_capture_at_stop(nullptr);
    niobium::compiler().set_post_recording_hook(&on_post_recording);
}

}  // namespace

extern "C" hazeError_t hazeReplayBridgeInitCryptoContext(
    uint64_t  ring_dim,
    uint64_t  desired_modulus,
    uint64_t* picked_modulus) noexcept {
    if (picked_modulus == nullptr) return HAZE_ERROR_INVALID_VALUE;

    try {
        auto rebuild = rebuild_context_locked(ring_dim, desired_modulus);
        if (!rebuild) return to_haze_error(rebuild.error());

        // capture_crypto_context populates ring dimension + modulus
        // chain AND serializes <program_dir>/cryptocontext.dat from
        // inside libnbfhetch's TU (where cereal type registrations
        // resolve). Also registers our post_recording_hook so the
        // bridge's input-bin / template synthesis runs at stop() time
        // without libnbfhetch needing to know about haze-specific
        // artifacts.
        push_crypto_to_compiler();

        *picked_modulus = g_ctx.picked_modulus;
        return HAZE_SUCCESS;
    } catch (const std::exception& e) {
        haze::log_error("replay_bridge",
                        std::string{"init threw: "} + e.what());
        return HAZE_ERROR_LAUNCH_FAILURE;
    } catch (...) {
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
}
