// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge implementation: synthesize the CryptoContext +
// per-input cereal-binary .bin files + per-output ciphertext templates that
// the compiler-side nbcc_fhetch_replay needs in order to write
// serialized_probes/ from a haze recording. See replay_bridge.h for the
// public API and architectural rationale.

#include <haze/replay_bridge.h>

// RYANPR: Lint that all of these includes are needed.
#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <niobium/compiler.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// libnbfhetch internal helpers used by the bridge. Forward-declared here
// rather than pulled in via "compiler_internal.h" because:
//
// RYANPR: This seems like something that could cause a drift when fhetch drifts. Maybe fhetch needs to make whatever parts public.
//   1. compiler_internal.h is part of libnbfhetch's PRIVATE include tree
//      (it lives under src/, not include/) and is intended for libnbfhetch's
//      own translation units. Including it from a downstream library would
//      add a dependency on libnbfhetch's source layout that the bridge's
//      target_include_directories doesn't currently express.
//
// RYANPR: This seems like a hack. Were these types added by our custom changes to FHETCH anyway? If so, we should look into how the niobium-client handles writing ciphertexts and if not modify our changes to make these APIs public.
//   2. The cereal polymorphic-type registry is per-shared-library —
//      calling lbcrypto::Serial::SerializeToFile from this TU directly
//      trips "unregistered polymorphic type (lbcrypto::CryptoContextImpl<...>)".
//      Routing the writes through libnbfhetch's own TU (where the auto-facade
//      already includes ciphertext-ser.h / cryptocontext-ser.h and forces
//      static initialisation) is the cleanest fix; the extern declarations
//      below reach those entry points without exposing the full Impl layout.
//
// If libnbfhetch ever publishes a stable C++ public API for these
// operations, replace these externs with that header include.
namespace niobium::detail {
bool write_ciphertext_template(
    const std::string& name,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct);
// RYANPR: Double check that the ciphertext bin is not just the serialized polynomial in some order (lsb/msb, which limb is which level, etc). We would remove the need for this if serialization is really just the raw bytes in order.
bool write_ciphertext_input_bin(
    const std::string& name,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
    const std::vector<uint64_t>& addr_ids);
// RYANPR: Add a description to what each of these functions is (for the ones you end up keeping)
void for_each_captured_input(
    const std::function<void(const std::string&, uint64_t, uint64_t,
                              const std::vector<uint64_t>&)>& cb);
void for_each_captured_output(
    const std::function<void(const std::string&)>& cb);
}  // namespace niobium::detail

namespace {

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// Per-process cache: same (ring_dim, desired_modulus) reuses the same
// CryptoContext + keypair across cryptocontext.dat, every template, and
// every input bin. OpenFHE resolves Ciphertext<->CryptoContext bindings
// through a static tag registry on deserialize; sharing the CC keeps the
// tag stable. The keypair is needed only as a structural carrier for
// Encrypt() — we never decrypt.
struct CachedContext {
    uint64_t ring_dim = 0;
    uint64_t desired_modulus = 0;
    uint64_t picked_modulus = 0;
    lbcrypto::CryptoContext<DCRTPoly> cc;
    lbcrypto::KeyPair<DCRTPoly> keys;
};

// RYANPR: Why do we need a mutex for this simple application? This seems overengineered.
CachedContext g_ctx;
std::mutex    g_ctx_mu;

// RYANPR: Since this is just bit_width name it bit_width_including_zero or something. I'm also not sure we ever get a modulus of zero; they should all be primes...
// Number of bits needed to represent `modulus` (i.e. `floor(log2(modulus))
// + 1`). Used to size the prime OpenFHE picks. C++20 std::bit_width
// handles edge cases (2^63 returns 64) that an iterative shift loop
// silently truncates.
uint32_t bits_of(uint64_t modulus) {
    return modulus == 0 ? 0 : static_cast<uint32_t>(std::bit_width(modulus));
}

// Build (or rebuild) the cached CryptoContext to match (ring_dim,
// desired_modulus). Returns true on success; populates g_ctx.
bool rebuild_context_locked(uint64_t ring_dim, uint64_t desired_modulus) {
    using namespace lbcrypto;
    // RYANPR: Make this return a better error type with an enum or whatever so we can know what the error is. The ring dimension check makes sense to me, but the modulus check I don't understand.
    if (ring_dim == 0 || desired_modulus < 2) return false;

    if (g_ctx.cc && g_ctx.ring_dim == ring_dim &&
        g_ctx.desired_modulus == desired_modulus) {
        return true;
    }

    CCParams<CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(HEStd_NotSet);  // toy: ring_dim is user-chosen
    p.SetRingDim(ring_dim);
    // RYANPR: Make sure to note a TODO that we are only handling a single residue polynomial at the moment but that we may need to do something else later (so more than depth 0)
    // Depth 0 produces a single-tower DCRTPoly chain so the synthesized
    // template ciphertext has exactly two NativePoly slots (one per
    // ciphertext element a, b). Trim element[1] before serializing
    // (see synthesize_haze_ciphertext below) to land on a 1-NativePoly
    // .bin matching haze's per-input addr_id count of one. Multi-residue
    // support will need a different shape — see plan in replay_bridge.h.
    p.SetMultiplicativeDepth(0);
    const uint32_t bits = bits_of(desired_modulus);
    p.SetScalingModSize(bits ? bits - 1 : 50);
    p.SetFirstModSize(bits ? bits : 60);
    // RYANPR: Fine for now, but why do we need this?
    p.SetScalingTechnique(FIXEDMANUAL);

    auto cc = GenCryptoContext(p);
    if (!cc) return false;
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    // Cached keypair: needed by Encrypt() to produce a structurally valid
    // Ciphertext<DCRTPoly>. One keygen per (ring_dim, desired_modulus)
    // pair, reused across every template and every input bin.
    // RYANPR: Do we actually need this? I assume the relay we are using asks for keys but also we are constructing dummy polynomials for our tests to see if basic operations work; they don't need encrypted values.
    auto keys = cc->KeyGen();
    if (!keys.publicKey || !keys.secretKey) return false;

    auto element_params = cc->GetCryptoParameters()->GetElementParams();
    if (!element_params || element_params->GetParams().empty()) return false;
    uint64_t picked = element_params->GetParams()[0]->GetModulus().ConvertToInt();

    g_ctx.ring_dim = ring_dim;
    g_ctx.desired_modulus = desired_modulus;
    g_ctx.picked_modulus = picked;
    g_ctx.cc = cc;
    g_ctx.keys = keys;

    // RYANPR: What does returning true mean
    return true;
}

// Align niobium::compiler()'s program_info to haze's defaults BEFORE
// reading get_program_directory().
// RYANPR: Not sure why you need this, the tests should be able to set their own program info and use that.
void align_program_info_to_haze() {
    niobium::compiler().set_program_info("haze", "0.1", "HAZE runtime");
}

// Build a structurally valid 1-element / 1-tower Ciphertext<DCRTPoly> and
// fill its first NativePoly with `values`. Caller passes pre-acquired
// g_ctx_mu.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_haze_ciphertext_locked(const std::vector<uint64_t>& values) {
    std::vector<double> zeros(g_ctx.ring_dim / 2, 0.0);
    auto pt = g_ctx.cc->MakeCKKSPackedPlaintext(zeros);
    // RYANPR: I don't think we need actual encryption for the test. I really just want to check that the polynomial operations work. We know the ciphertext is a pair of polynomials; we could fill one with the one we want and one with zeros.
    auto ct = g_ctx.cc->Encrypt(g_ctx.keys.publicKey, pt);
    // Trim the RLWE pair (a, b) down to a 1-element ciphertext so the
    // .bin's NativePoly count matches haze's one-polynomial-per-input
    // model. element[1] would otherwise land in the .bin as Encrypt-noise
    // and the compiler-side would auto-allocate a colliding addr_id for
    // it. els.resize(1) leaves the result not-decryptable, but we never
    // decrypt — the compiler-side reads NativePoly values positionally,
    // not via the CKKS API.
    {
        auto& els = ct->GetElements();
        if (els.size() > 1) els.resize(1);
    }
    // RYANPR: What are you guarding against here?
    if (!values.empty() &&
        ct->GetElements().size() > 0 &&
        ct->GetElements()[0].GetAllElements().size() > 0) {
        auto& dcrt = ct->GetElements()[0];
        auto& np = dcrt.GetAllElements()[0];
        size_t n = np.GetParams()->GetRingDimension();
        if (values.size() == n) {
            lbcrypto::NativeVector nv(
                n, lbcrypto::NativeInteger(np.GetModulus()));
            for (size_t i = 0; i < n; ++i)
                nv[i] = lbcrypto::NativeInteger(values[i]);
            np.SetValues(nv, np.GetFormat());
        } else {
            std::cerr << "[haze_replay_bridge] values.size()=" << values.size()
                      << " != ring_dim=" << n
                      << " — cannot fill input ciphertext" << std::endl;
        }
    }
    return ct;
}

// Post-recording hook. Synthesizes the per-program OpenFHE artifacts the
// compiler-side nbcc_fhetch_replay needs to consume haze's pure-FHETCH
// recording:
//
//   - For each captured input, a 1-element / 1-tower Ciphertext<DCRTPoly>
//     filled with the captured values, serialized as
//     <prog>.input_<name>.bin + .ids.
//   - For each captured output, an empty 1-element template serialized as
//     ciphertext_templates/<name>.template.
//
// Both iterations run AFTER trace_writer.stop_recording() (post_recording_hook
// fires there), so the OpenFHE Encrypt() inside synthesize_haze_ciphertext
// is recording-disabled and doesn't pollute the trace. Eval keys are
// skipped — they have a bespoke cereal path written by the auto-facade's
// serialize_eval_keys.
// RYANPR: Why do we need this?
void on_post_recording() {
    std::lock_guard<std::mutex> lock(g_ctx_mu);
    if (!g_ctx.cc) {
        // No CC initialized for this session — bridge was never called,
        // so there are no haze-style artifacts to materialize. Quiet no-op.
        return;
    }
    align_program_info_to_haze();

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
            auto ct = synthesize_haze_ciphertext_locked(acc.values);
            if (!niobium::detail::write_ciphertext_input_bin(
                    name, ct, acc.addr_ids)) {
                std::cerr << "[haze_replay_bridge] write_ciphertext_input_bin "
                          << "failed for '" << name << "'" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[haze_replay_bridge] input bin synthesis for '"
                      << name << "' threw: " << e.what() << std::endl;
        }
    }

    // ---- Outputs ----
    // Auto-write a 1-element template for each named output. The
    // compiler-side fills the first NativePoly with simulator output;
    // templates are otherwise empty. This removes the burden from test
    // code of having to call hazeReplayBridgeWriteTemplate per output —
    // tests just call hazeReplay() and the bridge handles it.
    niobium::detail::for_each_captured_output(
        [&](const std::string& name) {
            try {
                auto ct = synthesize_haze_ciphertext_locked({});
                if (!niobium::detail::write_ciphertext_template(name, ct)) {
                    std::cerr << "[haze_replay_bridge] write_ciphertext_template "
                              << "failed for '" << name << "'" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[haze_replay_bridge] template synthesis for '"
                          << name << "' threw: " << e.what() << std::endl;
            }
        });
}

// Push the bridge's CryptoContext into niobium::compiler() via the
// existing OpenFHE-aware capture path. capture_crypto_context lives
// inside libnbfhetch (auto_facade.cpp), so the cereal polymorphic
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
// immediately after capture so stop() doesn't fire it.
// RYANPR: I assume this is the fhetch compiler, not the real compiler.
void push_crypto_to_compiler_locked() {
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

    std::lock_guard<std::mutex> lock(g_ctx_mu);
    try {
        if (!rebuild_context_locked(ring_dim, desired_modulus))
            return HAZE_ERROR_INVALID_VALUE;

        align_program_info_to_haze();
        // capture_crypto_context populates ring dimension + modulus chain
        // AND serializes <program_dir>/cryptocontext.dat from inside
        // libnbfhetch's TU (where cereal type registrations resolve).
        // Also registers our post_recording_hook so write_haze_input_bins
        // runs at stop() time without libnbfhetch needing to know about
        // haze-specific artifacts.
        push_crypto_to_compiler_locked();

        *picked_modulus = g_ctx.picked_modulus;
        return HAZE_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[haze_replay_bridge] init threw: " << e.what()
                  << std::endl;
        return HAZE_ERROR_LAUNCH_FAILURE;
    } catch (...) {
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
}
