// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge implementation: synthesize the CryptoContext, per-input
// .bin files, and per-output templates the simulator / nbcc_fhetch_replay
// consume to emit serialized_probes/<name>.ct. Called only from the epoch
// path under EpochState::mutex_, so no internal lock is needed.

#include "ciphertext-ser.h"
#include "common/log.hpp"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "openfhe.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <map>
#include <memory>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility> // std::move, std::unreachable (C++23)
#include <vector>

// libnbfhetch internal helpers; forward-declared because compiler_internal.h
// is private to libnbfhetch's src/ tree. The detail symbols route cereal
// serialization through libnbfhetch's TU where polymorphic-type registrations
// live (inlining the calls throws "unregistered polymorphic type" on macOS).
namespace niobium::detail {

// Serialize a Ciphertext<DCRTPoly> shell to ciphertext_templates/<name>.template;
// the replay path fills it and re-serializes as serialized_probes/<name>.ct.
bool write_ciphertext_template(const std::string &name,
                               const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct);

// Serialize a populated Ciphertext<DCRTPoly> as the cereal-binary
// {.bin, .ids} pair the compiler-side fhetch_driver expects for an
// input named `name`.
bool write_ciphertext_input_bin(const std::string &name,
                                const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct,
                                const std::vector<uint64_t> &addr_ids);

// Fires once per distinct fhetch::tag_input with a full record snapshot
// (shape, addr_ids, per-residue values) so the bridge can dispatch on kind.
void for_each_captured_input(const std::function<void(const niobium::CapturedInputRecord &)> &cb);

// Iterate captured outputs; the bridge dispatches on shape.kind to pick the
// CT geometry (SRP=1×1, MRP=1×N, SRPArray=K×1, MRPArray=K×M_k).
void for_each_captured_output(
    const std::function<void(const std::string &, const niobium::CapturedShape &)> &cb);

} // namespace niobium::detail

namespace {

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// BridgeError surfaces specific failure modes from the C-ABI entry; lives
// in this TU because there's exactly one caller and one implementation.
enum class BridgeError : uint8_t {
    InvalidModulus,
    KeygenFailed,
    OpenFheUnavailable,
};

// Per-process CC cache: SRP outputs use a depth-0 single-tower CC, MRP
// outputs use depth-(N-1) chains, all sharing OpenFHE's static
// CC<->Ciphertext registry. picked_moduli records OpenFHE's chosen primes
// (it picks by bit-width); rewrite_tower_moduli swaps them back to the
// user-requested values at synthesis time.
struct CachedContext {
    uint64_t ring_dim = 0;
    std::vector<uint64_t> moduli;        // user-requested base
    std::vector<uint64_t> picked_moduli; // OpenFHE-picked primes per tower
    lbcrypto::CryptoContext<DCRTPoly> cc;
    lbcrypto::KeyPair<DCRTPoly> keys;
};

// Cache keyed by user-requested moduli; ring_dim is pinned per process via
// hazeSetRingDimension, and reset_bridge_caches wipes the cache on every
// hazeDeviceReset, so the moduli vector alone disambiguates entries.
std::map<std::vector<uint64_t>, CachedContext> g_ctx_cache;
// Primary CC moduli: the entry that init seeded and that captured
// cryptocontext.dat; nullopt before init / after reset_bridge_caches.
std::optional<std::vector<uint64_t>> g_primary_key;
// Program directory snapshotted at init so reset_bridge_caches's disk
// cleanup is independent of niobium::compiler().reset() ordering.
fs::path g_program_dir;

// Build or look up a CryptoContext + keypair: moduli.size() == 1 → depth-0
// single-tower CC; >1 → depth-(N-1) chain with one tower per residue. On
// success returns a stable g_ctx_cache pointer; on failure, a BridgeError.
std::expected<const CachedContext *, BridgeError>
get_or_build_context_locked(uint64_t ring_dim, const std::vector<uint64_t> &moduli) {
    using namespace lbcrypto;
    if (moduli.empty())
        return std::unexpected(BridgeError::InvalidModulus);

    auto it = g_ctx_cache.find(moduli);
    if (it != g_ctx_cache.end())
        return &it->second;

    CCParams<CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(HEStd_NotSet); // toy: ring_dim is user-chosen
    p.SetRingDim(static_cast<uint32_t>(ring_dim));
    // Chain depth d → d+1 towers in a freshly encrypted CT, so SRP wants 0
    // and MRP with N residues wants N-1; array shapes compose per-element.
    p.SetMultiplicativeDepth(static_cast<uint32_t>(moduli.size() - 1));
    // First-mod bit-width tracks the primary modulus; scaling-mod tracks the
    // smallest remaining modulus, minus 1 per OpenFHE's scalingModSize+1 rule.
    const auto first_bits = static_cast<uint32_t>(std::bit_width(moduli.front()));
    p.SetFirstModSize(first_bits);
    if (moduli.size() == 1) {
        p.SetScalingModSize(first_bits - 1);
    } else {
        auto min_bits = static_cast<uint32_t>(std::bit_width(moduli[1]));
        for (size_t i = 2; i < moduli.size(); ++i)
            min_bits = std::min(min_bits, static_cast<uint32_t>(std::bit_width(moduli[i])));
        p.SetScalingModSize(min_bits - 1);
    }
    // FIXEDMANUAL: these CTs are structural shells, never EvalMult'd; default
    // (FLEXIBLEAUTO) reshapes the chain during GenCryptoContext and breaks
    // the bridge's depth-(N-1)→N-tower assumption for MRP templates.
    p.SetScalingTechnique(FIXEDMANUAL);

    auto cc = GenCryptoContext(p);
    if (!cc)
        return std::unexpected(BridgeError::OpenFheUnavailable);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    // TODO(haze:no-keygen): keygen+Encrypt+trim is structural ballast since
    // OpenFHE has no public no-key Ciphertext<DCRTPoly> constructor.
    auto keys = cc->KeyGen();
    if (!keys.publicKey || !keys.secretKey)
        return std::unexpected(BridgeError::KeygenFailed);

    auto element_params = cc->GetCryptoParameters()->GetElementParams();
    if (!element_params || element_params->GetParams().empty())
        return std::unexpected(BridgeError::OpenFheUnavailable);
    std::vector<uint64_t> picked;
    picked.reserve(element_params->GetParams().size());
    for (const auto &ep : element_params->GetParams())
        picked.push_back(ep->GetModulus().ConvertToInt());

    CachedContext entry;
    entry.ring_dim = ring_dim;
    entry.moduli = moduli;
    entry.picked_moduli = std::move(picked);
    entry.cc = cc;
    entry.keys = keys;
    auto [ins, _] = g_ctx_cache.emplace(moduli, std::move(entry));
    return &ins->second;
}

// Convenience accessor for the primary CC; returns nullptr before init.
const CachedContext *primary_ctx() {
    if (!g_primary_key)
        return nullptr;
    auto it = g_ctx_cache.find(*g_primary_key);
    return (it == g_ctx_cache.end()) ? nullptr : &it->second;
}

// Drop the CC cache, primary key, and on-disk artifacts so each test starts
// clean; without the disk half, addr-derived names from prior tests pollute
// content-based MRP lookups. Best-effort: fs errors are logged, not raised.
void reset_bridge_caches() {
    g_ctx_cache.clear();
    g_primary_key.reset();
    if (g_program_dir.empty())
        return; // bridge never initialized; nothing on disk
    try {
        const auto cleanup = [&](const fs::path &sub) {
            std::error_code ec;
            fs::remove_all(g_program_dir / sub, ec);
            if (ec) {
                std::ostringstream body;
                body << "reset_bridge_caches: remove_all('" << (g_program_dir / sub).string()
                     << "') failed: " << ec.message();
                haze::log_error("replay_bridge", body.str());
            }
        };
        cleanup("serialized_probes");
        cleanup("ciphertext_templates");
    } catch (const std::exception &e) {
        haze::log_error("replay_bridge",
                        std::string{"reset_bridge_caches: disk cleanup threw: "} + e.what());
    } catch (...) {
        haze::log_error("replay_bridge", "reset_bridge_caches: disk cleanup threw (unknown)");
    }
}

// Map BridgeError → hazeError_t at the C ABI; granular variants exist for
// diagnostics, but the public surface is intentionally coarser.
hazeError_t to_haze_error(BridgeError e) {
    switch (e) {
    case BridgeError::InvalidModulus:
        return HAZE_ERROR_INVALID_VALUE;
    case BridgeError::KeygenFailed:
    case BridgeError::OpenFheUnavailable:
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_LAUNCH_FAILURE;
}

// Encrypt zeros and trim the RLWE pair to a 1-element shell; element[1]
// would otherwise land in the .bin as Encrypt-noise with a colliding
// auto-addr_id. TODO(haze:no-keygen): replace once OpenFHE exposes a
// no-key shell constructor.
lbcrypto::Ciphertext<DCRTPoly> empty_one_element_ct(const CachedContext &ctx) {
    std::vector<double> zeros(ctx.ring_dim / 2, 0.0);
    auto pt = ctx.cc->MakeCKKSPackedPlaintext(zeros);
    auto ct = ctx.cc->Encrypt(ctx.keys.publicKey, pt);
    auto &els = ct->GetElements();
    if (els.size() > 1)
        els.resize(1);
    return ct;
}

// Fill NativePoly `np` with `values` after validating the ring-dim
// match. Logs and leaves the polynomial untouched on size mismatch.
void fill_native_poly(lbcrypto::NativePoly &np, const std::vector<uint64_t> &values,
                      const std::string &diag) {
    size_t n = np.GetParams()->GetRingDimension();
    if (values.size() != n) {
        std::ostringstream body;
        body << diag << ": values.size()=" << values.size() << " != ring_dim=" << n;
        haze::log_error("replay_bridge", body.str());
        return;
    }
    lbcrypto::NativeVector nv(n, lbcrypto::NativeInteger(np.GetModulus()));
    for (size_t i = 0; i < n; ++i)
        nv[i] = lbcrypto::NativeInteger(values[i]);
    np.SetValues(nv, np.GetFormat());
}

// SRP entrypoint: build a 1-element / 1-tower CT and fill its NativePoly
// with `values` (empty `values` produces a template).
lbcrypto::Ciphertext<DCRTPoly> synthesize_haze_ciphertext(const CachedContext &ctx,
                                                          const std::vector<uint64_t> &values) {
    auto ct = empty_one_element_ct(ctx);
    if (values.empty())
        return ct; // template
    fill_native_poly(ct->GetElements()[0].GetAllElements()[0], values,
                     "synthesize_haze_ciphertext");
    return ct;
}

// Replace each tower's modulus with the user-requested value; OpenFHE picks
// bits-near primes that are invisible to SRP readers but visible to MRP
// (mrp.base() from GetModulus()), and the simulator preserves the
// template's modulus when emitting probes, so the override has to happen here.
void rewrite_tower_moduli(lbcrypto::DCRTPoly &dcrt, const std::vector<uint64_t> &user_moduli,
                          uint32_t ring_dim) {
    auto &towers = dcrt.GetAllElements();
    if (towers.size() != user_moduli.size()) {
        // CC was built for these moduli, so a mismatch means OpenFHE produced
        // an unexpected chain; log it before the loop silently truncates.
        std::ostringstream body;
        body << "rewrite_tower_moduli: tower count " << towers.size() << " != user_moduli.size() "
             << user_moduli.size();
        haze::log_error("replay_bridge", body.str());
    }
    for (size_t i = 0; i < towers.size() && i < user_moduli.size(); ++i) {
        const auto fmt = towers[i].GetFormat();
        auto params = std::make_shared<lbcrypto::ILNativeParams>(
            2 * ring_dim, lbcrypto::NativeInteger(user_moduli[i]));
        // initializeElementToZero=true populates a length-ring_dim NativeVector;
        // a later fill_native_poly overwrites with simulator-fed values.
        towers[i] = lbcrypto::NativePoly(params, fmt, /*initializeElementToZero=*/true);
    }
}

// Build a 1-element / N-tower CT for an MRP output (empty
// per_residue_values → template shell). Tower moduli are rewritten to the
// user base so result(name, MRP&) returns mrp.base() == moduli.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_haze_mrp_ciphertext(const CachedContext &ctx, const std::vector<uint64_t> &moduli,
                               const std::vector<std::vector<uint64_t>> &per_residue_values) {
    auto ct = empty_one_element_ct(ctx);
    auto &dcrt = ct->GetElements()[0];
    if (dcrt.GetAllElements().size() != moduli.size()) {
        // Throw rather than emit a wrong-shape template; on_post_recording
        // catches and skips this output instead of corrupting the trace.
        std::ostringstream body;
        body << "synthesize_haze_mrp_ciphertext: tower count " << dcrt.GetAllElements().size()
             << " != moduli.size() " << moduli.size() << " — CC was built with the wrong depth";
        throw std::runtime_error(body.str());
    }
    rewrite_tower_moduli(dcrt, moduli, static_cast<uint32_t>(ctx.ring_dim));
    if (per_residue_values.empty())
        return ct; // template — moduli were just installed; values stay zero.
    if (per_residue_values.size() != moduli.size()) {
        std::ostringstream body;
        body << "synthesize_haze_mrp_ciphertext: per_residue_values.size()="
             << per_residue_values.size() << " != moduli.size()=" << moduli.size();
        throw std::runtime_error(body.str());
    }
    auto &towers = dcrt.GetAllElements();
    for (size_t i = 0; i < towers.size(); ++i) {
        fill_native_poly(towers[i], per_residue_values[i],
                         "synthesize_haze_mrp_ciphertext[tower " + std::to_string(i) + "]");
    }
    return ct;
}

// Build a K-element CT for SRPArray/MRPArray; heterogeneous per-element
// bases are rejected since they'd need multi-CC stitching OpenFHE doesn't
// support. K independent Encrypts (typically K=2) avoid shallow-aliasing
// during fill and bypass the missing public no-key K-shell constructor.
lbcrypto::Ciphertext<DCRTPoly> synthesize_haze_array_ciphertext(
    const CachedContext &ctx, const std::vector<std::vector<uint64_t>> &per_element_moduli,
    const std::vector<std::vector<std::vector<uint64_t>>> &per_element_values) {
    if (per_element_moduli.empty())
        throw std::runtime_error("synthesize_haze_array_ciphertext: empty per_element_moduli");
    for (size_t k = 1; k < per_element_moduli.size(); ++k) {
        if (per_element_moduli[k] != per_element_moduli[0]) {
            std::ostringstream body;
            body << "synthesize_haze_array_ciphertext: heterogeneous "
                    "per_element_moduli not yet implemented (element[0].size()="
                 << per_element_moduli[0].size() << ", element[" << k
                 << "].size()=" << per_element_moduli[k].size() << ")";
            throw std::runtime_error(body.str());
        }
    }
    const auto &shared_moduli = per_element_moduli[0];
    const size_t k_count = per_element_moduli.size();

    std::vector<DCRTPoly> elements;
    elements.reserve(k_count);
    for (size_t k = 0; k < k_count; ++k) {
        auto ct_k = empty_one_element_ct(ctx);
        if (shared_moduli.size() > 1) {
            rewrite_tower_moduli(ct_k->GetElements()[0], shared_moduli,
                                 static_cast<uint32_t>(ctx.ring_dim));
        }
        elements.push_back(std::move(ct_k->GetElements()[0]));
    }
    auto ct = empty_one_element_ct(ctx);
    ct->SetElements(elements);

    if (per_element_values.empty())
        return ct; // template path: K-element shell, zeros in each tower
    if (per_element_values.size() != k_count) {
        std::ostringstream body;
        body << "synthesize_haze_array_ciphertext: per_element_values.size()="
             << per_element_values.size() << " != per_element_moduli.size()=" << k_count;
        throw std::runtime_error(body.str());
    }
    auto &els = ct->GetElements();
    for (size_t k = 0; k < els.size(); ++k) {
        const auto &row = per_element_values[k];
        if (row.size() != shared_moduli.size()) {
            std::ostringstream body;
            body << "synthesize_haze_array_ciphertext: element " << k << " has " << row.size()
                 << " residues, expected " << shared_moduli.size();
            throw std::runtime_error(body.str());
        }
        auto &towers = els[k].GetAllElements();
        for (size_t i = 0; i < towers.size(); ++i) {
            fill_native_poly(towers[i], row[i],
                             "synthesize_haze_array_ciphertext[elem " + std::to_string(k) +
                                 ", tower " + std::to_string(i) + "]");
        }
    }
    return ct;
}

// Pick the CC for a shape; arrays require homogeneous per-element moduli,
// rejected with a logged nullptr so callers needn't distinguish "TODO" from
// "synthesis failure". Modulus-0 falls back to the primary single-tower CC
// (TODO(niobium-fhetch:mod-map-tracking): fhetch's address_modulus_map
// doesn't register every output before sync_fhetch_state_to_compiler).
const CachedContext *context_for_shape(uint64_t ring_dim, const niobium::CapturedShape &shape) {
    if (shape.per_element_moduli.empty())
        return primary_ctx();
    const auto &moduli = shape.per_element_moduli.front();
    if (moduli.empty())
        return primary_ctx();
    for (auto q : moduli) {
        if (q == 0)
            return primary_ctx();
    }
    // Reject heterogeneous arrays here so the caller gets a structured
    // nullptr (mirrors the check in synthesize_haze_array_ciphertext —
    // change both together).
    if (shape.kind == niobium::CapturedKind::SRPArray ||
        shape.kind == niobium::CapturedKind::MRPArray) {
        for (size_t k = 1; k < shape.per_element_moduli.size(); ++k) {
            if (shape.per_element_moduli[k] != moduli) {
                std::ostringstream body;
                body << "context_for_shape: heterogeneous per_element_moduli "
                        "(element[0].size()="
                     << moduli.size() << ", element[" << k
                     << "].size()=" << shape.per_element_moduli[k].size()
                     << ") — array synthesis with non-uniform bases is not yet implemented";
                haze::log_error("replay_bridge", body.str());
                return nullptr;
            }
        }
    }
    auto built = get_or_build_context_locked(ring_dim, moduli);
    if (!built) {
        std::ostringstream body;
        body << "context_for_shape: failed to build CC for ring_dim=" << ring_dim
             << " (BridgeError=" << static_cast<int>(built.error()) << ")";
        haze::log_error("replay_bridge", body.str());
        // primary_ctx() here would silently emit a 1-tower template for an
        // N-tower output; nullptr lets the caller skip this output instead.
        return nullptr;
    }
    return *built;
}

// Reshape flat per-residue values into per-element rows using
// per_element_moduli sizes as boundaries; used by the array dispatch below.
std::vector<std::vector<std::vector<uint64_t>>>
slice_array_values(const std::vector<std::vector<uint64_t>> &per_residue_values,
                   const std::vector<std::vector<uint64_t>> &per_element_moduli) {
    std::size_t expected = 0;
    for (const auto &m : per_element_moduli)
        expected += m.size();
    if (per_residue_values.size() != expected) {
        // Mismatch implies a bug in fhetch's CapturedInputRecord; surface
        // it here so fill_native_poly's per-row warnings don't drown the cause.
        std::ostringstream body;
        body << "slice_array_values: per_residue_values.size()=" << per_residue_values.size()
             << " != sum of per_element_moduli sizes (" << expected << ")";
        haze::log_error("replay_bridge", body.str());
    }
    std::vector<std::vector<std::vector<uint64_t>>> out;
    out.reserve(per_element_moduli.size());
    size_t cursor = 0;
    for (const auto &elem_moduli : per_element_moduli) {
        std::vector<std::vector<uint64_t>> row;
        row.reserve(elem_moduli.size());
        for (size_t i = 0; i < elem_moduli.size(); ++i) {
            if (cursor < per_residue_values.size())
                row.push_back(per_residue_values[cursor]);
            else
                row.emplace_back();
            ++cursor;
        }
        out.push_back(std::move(row));
    }
    return out;
}

// Build the CT shell for `shape`; empty per_residue_values yields a
// template, non-empty fills with values flattened in encounter order.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_for_shape(const CachedContext &ctx, const niobium::CapturedShape &shape,
                     const std::vector<std::vector<uint64_t>> &per_residue_values) {
    switch (shape.kind) {
    case niobium::CapturedKind::SRP:
        return synthesize_haze_ciphertext(
            ctx, per_residue_values.empty() ? std::vector<uint64_t>{} : per_residue_values.front());
    case niobium::CapturedKind::MRP:
        return synthesize_haze_mrp_ciphertext(ctx, shape.per_element_moduli.front(),
                                              per_residue_values);
    case niobium::CapturedKind::SRPArray:
    case niobium::CapturedKind::MRPArray:
        return synthesize_haze_array_ciphertext(
            ctx, shape.per_element_moduli,
            per_residue_values.empty()
                ? std::vector<std::vector<std::vector<uint64_t>>>{}
                : slice_array_values(per_residue_values, shape.per_element_moduli));
    }
    std::unreachable();
}

// LOAD-BEARING. Synthesizes the OpenFHE artifacts the replay path needs;
// runs after stop_recording (so for_each_captured_* sees the final sets)
// and before reconstruct (so cereal registrations resolve).
void on_post_recording() {
    const auto *primary = primary_ctx();
    if (primary == nullptr) {
        // Bridge never called this session; nothing to materialize.
        return;
    }
    const uint64_t ring_dim = primary->ring_dim;

    // ---- Inputs ----
    niobium::detail::for_each_captured_input([&](const niobium::CapturedInputRecord &rec) {
        // Keys aren't shape-tagged yet (Tier 2 / FIDESlib); skip so we
        // don't synthesize a key-shaped CT from per-poly tags.
        if (rec.name == "evalmult_key" || rec.name == "automorphism_key")
            return;
        try {
            const auto *ctx = context_for_shape(ring_dim, rec.shape);
            if (ctx == nullptr) {
                haze::log_error("replay_bridge",
                                "input '" + rec.name + "': no CC available for shape");
                return;
            }
            auto ct = synthesize_for_shape(*ctx, rec.shape, rec.per_residue_values);
            if (!niobium::detail::write_ciphertext_input_bin(rec.name, ct, rec.addr_ids)) {
                haze::log_error("replay_bridge",
                                "write_ciphertext_input_bin failed for '" + rec.name + "'");
            }
        } catch (const std::exception &e) {
            haze::log_error("replay_bridge",
                            "input bin synthesis for '" + rec.name + "' threw: " + e.what());
        }
    });

    // ---- Outputs ----
    // Auto-write a template per output; the replay path fills in values
    // when emitting serialized_probes/<name>.ct.
    niobium::detail::for_each_captured_output(
        [&](const std::string &name, const niobium::CapturedShape &shape) {
            try {
                const auto *ctx = context_for_shape(ring_dim, shape);
                if (ctx == nullptr) {
                    haze::log_error("replay_bridge",
                                    "output '" + name + "': no CC available for shape");
                    return;
                }
                auto ct = synthesize_for_shape(*ctx, shape, /*per_residue_values=*/{});
                if (!niobium::detail::write_ciphertext_template(name, ct)) {
                    haze::log_error("replay_bridge",
                                    "write_ciphertext_template failed for '" + name + "'");
                }
            } catch (const std::exception &e) {
                haze::log_error("replay_bridge",
                                "template synthesis for '" + name + "' threw: " + e.what());
            }
        });
}

// Push the CC through libnbfhetch's capture path so cereal registrations
// resolve in libnbfhetch's TU. Clear the auto_capture_at_stop bootstrap
// hook (haze has no bootstrap precompute) and install on_post_recording
// as the post_recording_hook.
void push_crypto_to_compiler(const CachedContext &ctx) {
    niobium::compiler().capture_crypto_context(ctx.cc);
    niobium::compiler().set_auto_capture_at_stop(nullptr);
    niobium::compiler().set_post_recording_hook(&on_post_recording);
    // Snapshot the program directory while the compiler still holds it,
    // so reset_bridge_caches's disk cleanup is ordering-independent.
    g_program_dir = niobium::compiler().get_program_directory();
}

} // namespace

extern "C" hazeError_t hazeReplayBridgeInitCryptoContext(uint64_t ring_dim,
                                                         uint64_t desired_modulus,
                                                         uint64_t *picked_modulus) noexcept {
    if (picked_modulus == nullptr)
        return HAZE_ERROR_INVALID_VALUE;

    try {
        // Seed the primary depth-0 CC used by SRP outputs and cryptocontext.dat;
        // MRP / array CCs are added on-demand from on_post_recording.
        std::vector<uint64_t> primary_moduli = {desired_modulus};
        auto built = get_or_build_context_locked(ring_dim, primary_moduli);
        if (!built)
            return to_haze_error(built.error());

        g_primary_key = std::move(primary_moduli);

        // capture_crypto_context populates ring/chain and serializes
        // cryptocontext.dat inside libnbfhetch's TU; push_crypto_to_compiler
        // also installs our post_recording_hook for stop()-time synthesis.
        push_crypto_to_compiler(**built);

        *picked_modulus = (*built)->picked_moduli.front();
        return HAZE_SUCCESS;
    } catch (const std::exception &e) {
        haze::log_error("replay_bridge", std::string{"init threw: "} + e.what());
        return HAZE_ERROR_LAUNCH_FAILURE;
    } catch (...) {
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
}

extern "C" void hazeReplayBridgeReset() noexcept {
    // Drop the CC cache and primary key so prior (ring_dim, moduli) entries
    // don't accumulate and stress OpenFHE's static CC<->Ciphertext registry;
    // hooks are cleared separately via niobium::compiler().reset().
    reset_bridge_caches();
}

// ============================================================================
// fhetch::result() overloads — read serialized_probes/<name>.ct as
// fhetch::Polynomial / MRP / MRPArray. Marked default visibility because the
// bridge dylib is built with CXX_VISIBILITY_PRESET=hidden.
// ============================================================================

#define NIOBIUM_FHETCH_RESULT_API __attribute__((visibility("default")))

namespace niobium::fhetch {

namespace {

// Convert OpenFHE's Format (utils/inttypes.h, global enum) to fhetch::Format.
inline Format openfhe_to_fhetch_format(::Format fmt) {
    return fmt == ::Format::COEFFICIENT ? Format::Coefficient : Format::Evaluation;
}

// Load <program_dir>/serialized_probes/<name>.ct as a Ciphertext<DCRTPoly>.
// Returns nullptr on any I/O / parse error.
lbcrypto::Ciphertext<DCRTPoly> load_serialized_probe(const std::string &name) {
    auto dir = niobium::compiler().get_program_directory();
    auto ct_path = dir / "serialized_probes" / (name + ".ct");
    if (!std::filesystem::exists(ct_path)) {
        haze::log_error("fhetch::result", "'" + name + "' not found at " + ct_path.string());
        return nullptr;
    }
    lbcrypto::Ciphertext<DCRTPoly> ct;
    if (!lbcrypto::Serial::DeserializeFromFile(ct_path.string(), ct, lbcrypto::SerType::BINARY)) {
        haze::log_error("fhetch::result", "failed to deserialize " + ct_path.string());
        return nullptr;
    }
    return ct;
}

// Pull a single residue's coefficients out of a NativePoly as
// std::vector<uint64_t>.
std::vector<uint64_t> native_poly_values(const lbcrypto::NativePoly &np) {
    const auto &vals = np.GetValues();
    const size_t n = vals.GetLength();
    std::vector<uint64_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(vals[i].ConvertToInt());
    }
    return out;
}

} // namespace

NIOBIUM_FHETCH_RESULT_API
bool result(const std::string &name, Polynomial &p) {
    auto ct = load_serialized_probe(name);
    if (!ct)
        return false;

    const auto &elements = ct->GetElements();
    if (elements.empty()) {
        haze::log_error("fhetch::result", "'" + name + "' has no DCRTPoly elements");
        return false;
    }
    const auto &towers = elements[0].GetAllElements();
    if (towers.empty()) {
        haze::log_error("fhetch::result", "'" + name + "' has no NativePoly towers");
        return false;
    }
    const auto &np = towers[0];
    auto values = native_poly_values(np);
    p = Polynomial::from_data(values, values.size(), openfhe_to_fhetch_format(np.GetFormat()));
    return true;
}

NIOBIUM_FHETCH_RESULT_API
bool result(const std::string &name, MRP &m) {
    auto ct = load_serialized_probe(name);
    if (!ct)
        return false;

    const auto &elements = ct->GetElements();
    if (elements.empty())
        return false;
    const auto &towers = elements[0].GetAllElements();
    if (towers.empty())
        return false;

    std::vector<std::pair<Polynomial, uint64_t>> pairs;
    pairs.reserve(towers.size());
    for (const auto &np : towers) {
        auto values = native_poly_values(np);
        auto poly =
            Polynomial::from_data(values, values.size(), openfhe_to_fhetch_format(np.GetFormat()));
        pairs.emplace_back(std::move(poly), np.GetModulus().ConvertToInt());
    }
    m = MRP::from_pairs(pairs);
    return true;
}

NIOBIUM_FHETCH_RESULT_API
bool result(const std::string &name, MRPArray &arr) {
    auto ct = load_serialized_probe(name);
    if (!ct)
        return false;

    const auto &elements = ct->GetElements();
    MRPArray out(elements.size());
    for (size_t i = 0; i < elements.size(); ++i) {
        const auto &dcrt = elements[i];
        const auto &towers = dcrt.GetAllElements();
        std::vector<std::pair<Polynomial, uint64_t>> pairs;
        pairs.reserve(towers.size());
        for (const auto &np : towers) {
            auto values = native_poly_values(np);
            auto poly = Polynomial::from_data(values, values.size(),
                                              openfhe_to_fhetch_format(np.GetFormat()));
            pairs.emplace_back(std::move(poly), np.GetModulus().ConvertToInt());
        }
        out[i] = MRP::from_pairs(pairs);
    }
    arr = std::move(out);
    return true;
}

} // namespace niobium::fhetch
