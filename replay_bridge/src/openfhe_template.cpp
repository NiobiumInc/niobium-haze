// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge implementation: synthesize the CryptoContext, per-input
// .bin files, and per-output templates the simulator / nbcc_fhetch_replay
// consume to emit serialized_probes/<name>.ct. The bridge holds NO
// file-scope state — everything the post-recording hook needs is captured
// into its lambda at install time.
//
// Setup entries (Init/Register) are called from test setup outside any
// lock; on_post_recording runs under EpochState::mutex_. No internal lock
// is needed because setup completes before any compute begins and the
// hook's state is owned lexically by libnbfhetch's post_recording_hook slot.

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
#include <haze/replay_bridge_cc.hpp>
#include <map>
#include <memory>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
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
    OpenFheUnavailable,
};

// A built CryptoContext plus the moduli used to build it. Carries no keys —
// the bridge does not call Encrypt anywhere; CT shells are built directly
// via CiphertextImpl + DCRTPoly + SetElements.
struct Context {
    uint64_t ring_dim = 0;
    // User-requested base. Populated when the bridge built the CC itself
    // (Init / context_for_shape per-shape build); left empty on the
    // Register* path where the caller hands in a finished CC and we never
    // consult the original moduli list.
    std::vector<uint64_t> moduli;
    std::vector<uint64_t> picked_moduli; // OpenFHE-picked primes per tower
    lbcrypto::CryptoContext<DCRTPoly> cc;
};

// Build a CryptoContext from (ring_dim, moduli). Pure function: no cache,
// no globals. moduli.size() == 1 → depth-0 single-tower CC; >1 →
// depth-(N-1) chain with one tower per residue. The caller decides whether
// to reuse the result.
std::expected<Context, BridgeError> build_context(uint64_t ring_dim,
                                                  const std::vector<uint64_t> &moduli) {
    using namespace lbcrypto;
    if (moduli.empty())
        return std::unexpected(BridgeError::InvalidModulus);

    CCParams<CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(HEStd_NotSet); // toy: ring_dim is user-chosen
    p.SetRingDim(static_cast<uint32_t>(ring_dim));
    // Chain depth d → d+1 towers in a freshly built CT, so SRP wants 0
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

    auto element_params = cc->GetCryptoParameters()->GetElementParams();
    if (!element_params || element_params->GetParams().empty())
        return std::unexpected(BridgeError::OpenFheUnavailable);

    Context entry;
    entry.ring_dim = ring_dim;
    entry.moduli = moduli;
    entry.picked_moduli.reserve(element_params->GetParams().size());
    for (const auto &ep : element_params->GetParams())
        entry.picked_moduli.push_back(ep->GetModulus().ConvertToInt());
    entry.cc = std::move(cc);
    return entry;
}

// Map BridgeError → hazeError_t at the C ABI; granular variants exist for
// diagnostics, but the public surface is intentionally coarser.
hazeError_t to_haze_error(BridgeError e) {
    switch (e) {
    case BridgeError::InvalidModulus:
        return HAZE_ERROR_INVALID_VALUE;
    case BridgeError::OpenFheUnavailable:
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_LAUNCH_FAILURE;
}

// Drop trailing towers from `dcrt` so it carries exactly `target` towers.
// `current < target` is a hard error: the CC was built too shallow for
// this output and silently shrinking the shape would corrupt the trace.
// `current > target` is the load-bearing path for the Register* user-CC
// flow, where the caller's CC has a deep chain (depth N) and a smaller-
// residue output (M < N+1 towers) needs its template trimmed.
void trim_towers_to(lbcrypto::DCRTPoly &dcrt, size_t target) {
    auto current = dcrt.GetAllElements().size();
    if (current < target) {
        std::ostringstream body;
        body << "trim_towers_to: CC chain has " << current << " towers, need " << target;
        throw std::runtime_error(body.str());
    }
    if (current > target)
        dcrt.DropLastElements(current - target);
}

// Build a CT shell with K zero-initialized DCRTPoly elements. K==1 covers
// SRP/MRP; K>=2 covers SRPArray/MRPArray. No Encrypt, no keys — the public
// CiphertextImpl(cc, ...) constructor + SetElements is all we need, and
// reconstruct_probes overwrites every NativePoly before re-serializing.
// Each DCRTPoly is sized by the CC's full chain; per-output tower trim
// happens in the synthesize_* dispatch via trim_towers_to.
lbcrypto::Ciphertext<DCRTPoly> empty_ct_shell(const Context &ctx, size_t k_count = 1) {
    auto element_params = ctx.cc->GetCryptoParameters()->GetElementParams();
    std::vector<DCRTPoly> elements;
    elements.reserve(k_count);
    for (size_t i = 0; i < k_count; ++i)
        elements.emplace_back(element_params, Format::EVALUATION, /*initializeElementToZero=*/true);
    auto ct = std::make_shared<lbcrypto::CiphertextImpl<DCRTPoly>>(ctx.cc, /*id=*/std::string{},
                                                                   lbcrypto::CKKS_PACKED_ENCODING);
    ct->SetElements(std::move(elements));
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
lbcrypto::Ciphertext<DCRTPoly> synthesize_haze_ciphertext(const Context &ctx,
                                                          const std::vector<uint64_t> &values) {
    auto ct = empty_ct_shell(ctx, /*k_count=*/1);
    trim_towers_to(ct->GetElements()[0], 1);
    if (values.empty())
        return ct; // template
    fill_native_poly(ct->GetElements()[0].GetAllElements()[0], values,
                     "synthesize_haze_ciphertext");
    return ct;
}

// Build a 1-element / N-tower CT for an MRP output (empty
// per_residue_values → template shell). The template's tower moduli are
// OpenFHE's picks; reconstruct_probes overwrites them with the trace's
// per-residue moduli before serializing the on-disk .ct.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_haze_mrp_ciphertext(const Context &ctx, const std::vector<uint64_t> &moduli,
                               const std::vector<std::vector<uint64_t>> &per_residue_values) {
    auto ct = empty_ct_shell(ctx, /*k_count=*/1);
    auto &dcrt = ct->GetElements()[0];
    // Trim trailing towers when the CC was built deeper than this output
    // needs (Register* path with a deep user CC). Throws on under-sized.
    trim_towers_to(dcrt, moduli.size());
    if (per_residue_values.empty())
        return ct; // template — reconstruct_probes installs trace moduli.
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
// support.
lbcrypto::Ciphertext<DCRTPoly> synthesize_haze_array_ciphertext(
    const Context &ctx, const std::vector<std::vector<uint64_t>> &per_element_moduli,
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

    auto ct = empty_ct_shell(ctx, k_count);
    // Trim each element's tower count to match the per-element residue
    // count (Register* path with a deep user CC).
    for (auto &dcrt : ct->GetElements())
        trim_towers_to(dcrt, shared_moduli.size());
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
synthesize_for_shape(const Context &ctx, const niobium::CapturedShape &shape,
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

// State the post-recording hook needs, captured into its lambda by Init or
// Register. Lives lexically inside the std::function set on niobium::compiler()
// and is freed when the next hook replaces it or niobium::compiler().reset()
// is called.
struct HookCtx {
    uint64_t ring_dim;
    Context primary;
    // When true (Register* path), use `primary` for every captured shape and
    // skip per-shape CC builds. When false (Init path), `primary` is only
    // the fallback for shapes whose moduli we can't resolve; per-shape CCs
    // get built lazily into the per-call local cache.
    bool primary_covers_all_shapes;
};

// Pick the CC for a shape. `local_cache` lives in `on_post_recording`'s
// frame so successive inputs/outputs with the same moduli share one build;
// the cache evaporates at hook exit. Heterogeneous arrays are rejected with
// a logged nullptr so callers needn't distinguish "TODO" from "synthesis
// failure". Modulus-0 falls back to `primary` (TODO(niobium-fhetch:
// mod-map-tracking): fhetch's address_modulus_map doesn't register every
// output before sync_fhetch_state_to_compiler).
const Context *context_for_shape(const HookCtx &hctx,
                                 std::map<std::vector<uint64_t>, Context> &local_cache,
                                 const niobium::CapturedShape &shape) {
    if (hctx.primary_covers_all_shapes)
        return &hctx.primary;
    if (shape.per_element_moduli.empty())
        return &hctx.primary;
    const auto &moduli = shape.per_element_moduli.front();
    if (moduli.empty())
        return &hctx.primary;
    for (auto q : moduli) {
        if (q == 0)
            return &hctx.primary;
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
    auto it = local_cache.find(moduli);
    if (it != local_cache.end())
        return &it->second;
    auto built = build_context(hctx.ring_dim, moduli);
    if (!built) {
        std::ostringstream body;
        body << "context_for_shape: failed to build CC for ring_dim=" << hctx.ring_dim
             << " (BridgeError=" << static_cast<int>(built.error()) << ")";
        haze::log_error("replay_bridge", body.str());
        // Returning &hctx.primary here would silently emit a 1-tower template
        // for an N-tower output; nullptr lets the caller skip this output.
        return nullptr;
    }
    auto [ins, _] = local_cache.emplace(moduli, std::move(*built));
    return &ins->second;
}

// LOAD-BEARING. Synthesizes the OpenFHE artifacts the replay path needs;
// runs after stop_recording (so for_each_captured_* sees the final sets)
// and before reconstruct (so cereal registrations resolve). `hctx` is
// captured by the post_recording_hook lambda; `local_cache` is reborn each
// call so no state leaks between epochs.
void on_post_recording(const HookCtx &hctx) {
    std::map<std::vector<uint64_t>, Context> local_cache;

    // ---- Inputs ----
    niobium::detail::for_each_captured_input([&](const niobium::CapturedInputRecord &rec) {
        // Keys aren't shape-tagged yet (Tier 2 / FIDESlib); skip so we
        // don't synthesize a key-shaped CT from per-poly tags.
        if (rec.name == "evalmult_key" || rec.name == "automorphism_key")
            return;
        try {
            const auto *ctx = context_for_shape(hctx, local_cache, rec.shape);
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
                const auto *ctx = context_for_shape(hctx, local_cache, shape);
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

// Install the post-recording hook on niobium::compiler(), moving `hctx`
// into the lambda closure. Also clears the auto_capture_at_stop bootstrap
// hook (haze has no bootstrap precompute). The lambda lives until the next
// install replaces it or niobium::compiler().reset() drops it.
void install_post_recording_hook(HookCtx hctx) {
    niobium::compiler().set_auto_capture_at_stop(nullptr);
    niobium::compiler().set_post_recording_hook([hctx = std::move(hctx)]() noexcept {
        try {
            on_post_recording(hctx);
        } catch (const std::exception &e) {
            haze::log_error("replay_bridge", std::string{"post_recording_hook threw: "} + e.what());
        } catch (...) {
            haze::log_error("replay_bridge", "post_recording_hook threw (unknown)");
        }
    });
}

} // namespace

extern "C" hazeError_t hazeReplayBridgeInitCryptoContext(uint64_t ring_dim,
                                                         uint64_t desired_modulus,
                                                         uint64_t *picked_modulus) noexcept {
    if (picked_modulus == nullptr)
        return HAZE_ERROR_INVALID_VALUE;

    try {
        auto built = build_context(ring_dim, {desired_modulus});
        if (!built)
            return to_haze_error(built.error());

        *picked_modulus = built->picked_moduli.front();

        // Plant program_name="haze" before capture writes cryptocontext.dat
        // so it lands in the eventual haze/ program directory rather than
        // the "niobium_trace" default fallback (libhaze's backend init runs
        // later, on the first EpochSession, and sets the same value
        // idempotently). capture_crypto_context serializes cryptocontext.dat
        // inside libnbfhetch's TU; subsequent Register*/re-init overwrites it.
        niobium::compiler().set_program_info("haze", "", "");
        niobium::compiler().capture_crypto_context(built->cc);

        install_post_recording_hook(HookCtx{
            .ring_dim = ring_dim,
            .primary = std::move(*built),
            .primary_covers_all_shapes = false,
        });
        return HAZE_SUCCESS;
    } catch (const std::exception &e) {
        haze::log_error("replay_bridge", std::string{"init threw: "} + e.what());
        return HAZE_ERROR_LAUNCH_FAILURE;
    } catch (...) {
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
}

extern "C" void hazeReplayBridgeReset() noexcept {
    // No in-memory state: the only thing to clean up is the on-disk
    // remnants from prior epochs so addr-derived template/probe names from
    // earlier tests don't pollute content-based MRP lookups. The
    // post-recording-hook lambda is freed by niobium::compiler().reset(),
    // which lifecycle.cpp::reset_all() invokes immediately after us.
    //
    // program_dir is queried live; reset_all() runs us before
    // niobium::compiler().reset(), so the directory is still valid.
    fs::path program_dir;
    try {
        program_dir = niobium::compiler().get_program_directory();
    } catch (...) {
        return;
    }
    if (program_dir.empty())
        return;

    try {
        for (const auto *sub : {"serialized_probes", "ciphertext_templates"}) {
            std::error_code ec;
            fs::remove_all(program_dir / sub, ec);
            if (ec) {
                std::ostringstream body;
                body << "hazeReplayBridgeReset: remove_all('" << (program_dir / sub).string()
                     << "') failed: " << ec.message();
                haze::log_error("replay_bridge", body.str());
            }
        }
    } catch (const std::exception &e) {
        haze::log_error("replay_bridge",
                        std::string{"hazeReplayBridgeReset: disk cleanup threw: "} + e.what());
    } catch (...) {
        haze::log_error("replay_bridge", "hazeReplayBridgeReset: disk cleanup threw (unknown)");
    }
}

namespace haze {

HAZE_API hazeError_t
hazeReplayBridgeRegisterCryptoContext(const lbcrypto::CryptoContext<DCRTPoly> &cc) noexcept {
    if (!cc)
        return HAZE_ERROR_INVALID_VALUE;
    try {
        auto element_params = cc->GetCryptoParameters()->GetElementParams();
        if (!element_params || element_params->GetParams().empty())
            return HAZE_ERROR_INVALID_VALUE;

        Context user_ctx;
        user_ctx.ring_dim = element_params->GetRingDimension();
        user_ctx.cc = cc;
        user_ctx.picked_moduli.reserve(element_params->GetParams().size());
        for (const auto &ep : element_params->GetParams())
            user_ctx.picked_moduli.push_back(ep->GetModulus().ConvertToInt());
        // No user-requested base — the caller built the CC themselves and
        // we never use Context::moduli when primary_covers_all_shapes is true.

        // Plant program_name="haze" before capture writes cryptocontext.dat;
        // see hazeReplayBridgeInitCryptoContext for the rationale.
        niobium::compiler().set_program_info("haze", "", "");
        niobium::compiler().capture_crypto_context(cc);

        install_post_recording_hook(HookCtx{
            .ring_dim = user_ctx.ring_dim,
            .primary = std::move(user_ctx),
            .primary_covers_all_shapes = true,
        });
        return HAZE_SUCCESS;
    } catch (const std::exception &e) {
        log_error("replay_bridge", std::string{"register_crypto_context threw: "} + e.what());
        return HAZE_ERROR_LAUNCH_FAILURE;
    } catch (...) {
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
}

} // namespace haze

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
