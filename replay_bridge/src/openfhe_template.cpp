// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// haze_replay_bridge implementation: synthesize the CryptoContext, per-input
// .bin files, and per-output templates the replay path consumes. Stateless —
// the post-recording hook's state lives lexically inside its lambda; setup
// runs outside any lock, on_post_recording runs under EpochState::mutex_.

#include "ciphertext-ser.h"
#include "common/log.hpp"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "openfhe.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <algorithm>
#include <atomic>
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

// libnbfhetch internal helpers forward-declared here so cereal polymorphic-type
// registrations resolve in libnbfhetch's TU (inlining throws on macOS).
namespace niobium::detail {

// Serialize a CT shell to ciphertext_templates/<name>.template for the
// replay path to fill and re-serialize as serialized_probes/<name>.ct.
bool write_ciphertext_template(const std::string &name,
                               const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct);

// Serialize a populated CT as the {.bin, .ids} pair fhetch_driver expects
// for an input named `name`.
bool write_ciphertext_input_bin(const std::string &name,
                                const lbcrypto::Ciphertext<lbcrypto::DCRTPoly> &ct,
                                const std::vector<uint64_t> &addr_ids);

// Iterate every distinct fhetch::tag_input record (shape, addr_ids, values).
void for_each_captured_input(const std::function<void(const niobium::CapturedInputRecord &)> &cb);

// Iterate every captured output; shape.kind picks the CT geometry.
void for_each_captured_output(
    const std::function<void(const std::string &, const niobium::CapturedShape &)> &cb);

} // namespace niobium::detail

namespace {

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// Granular failure variants for diagnostics; the C ABI collapses them.
enum class BridgeError : uint8_t {
    InvalidModulus,
    OpenFheUnavailable,
};

// Flag the post-recording hook sets on any per-record failure; haze
// drains it via hazeReplayBridgeTakeHookHadError after stop_epoch.
std::atomic<bool> &hook_had_error_flag() noexcept {
    static std::atomic<bool> flag{false};
    return flag;
}

void log_hook_error(const std::string &body) noexcept {
    haze::log_error("replay_bridge", body);
    hook_had_error_flag().store(true, std::memory_order_relaxed);
}

// Just the CC and its ring_dim; moduli come from the captured shape at
// hook time, picked primes are read off the CC when needed.
struct Context {
    uint64_t ring_dim = 0;
    lbcrypto::CryptoContext<DCRTPoly> cc;
};

// Pure builder: moduli.size()==1 yields a depth-0 single-tower CC, >1 yields
// a depth-(N-1) chain with one tower per residue.
std::expected<Context, BridgeError> build_context(uint64_t ring_dim,
                                                  const std::vector<uint64_t> &moduli) {
    using namespace lbcrypto;
    if (moduli.empty())
        return std::unexpected(BridgeError::InvalidModulus);

    CCParams<CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(HEStd_NotSet); // toy: ring_dim is user-chosen
    p.SetRingDim(static_cast<uint32_t>(ring_dim));
    // Chain depth d → d+1 towers per CT, matching one tower per residue.
    p.SetMultiplicativeDepth(static_cast<uint32_t>(moduli.size() - 1));
    // First-mod tracks the primary; scaling-mod tracks the smallest, -1
    // per OpenFHE's scalingModSize+1 rule.
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

    auto cc = GenCryptoContext(p);
    if (!cc)
        return std::unexpected(BridgeError::OpenFheUnavailable);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    auto element_params = cc->GetCryptoParameters()->GetElementParams();
    if (!element_params || element_params->GetParams().empty())
        return std::unexpected(BridgeError::OpenFheUnavailable);

    return Context{.ring_dim = ring_dim, .cc = std::move(cc)};
}

// First-tower modulus of `cc`'s chain. Throws on empty chain; build_context
// already validates non-empty so this is defensive for the Register* path.
uint64_t first_tower_modulus(const lbcrypto::CryptoContext<DCRTPoly> &cc) {
    const auto &params = cc->GetCryptoParameters()->GetElementParams()->GetParams();
    if (params.empty())
        throw std::runtime_error("first_tower_modulus: empty element params");
    return params.front()->GetModulus().ConvertToInt();
}

// Collapse BridgeError to the coarser C-ABI surface.
hazeError_t to_haze_error(BridgeError e) {
    switch (e) {
    case BridgeError::InvalidModulus:
        return HAZE_ERROR_INVALID_VALUE;
    case BridgeError::OpenFheUnavailable:
        return HAZE_ERROR_LAUNCH_FAILURE;
    }
    return HAZE_ERROR_LAUNCH_FAILURE;
}

// Drop trailing towers from `dcrt` to exactly `target`; throws if the CC
// chain is too shallow. Load-bearing for the Register* deep-CC path.
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

// Build a K-element CT shell sized by the CC's full chain; the
// synthesize_* dispatch trims per output. No Encrypt, no keys.
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

// Fill NativePoly `np` with `values`; logs and no-ops on ring-dim mismatch.
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

// SRP: 1-element / 1-tower CT; empty `values` produces a template.
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

// MRP: 1-element / N-tower CT; empty per_residue_values produces a template
// whose tower moduli reconstruct_probes overwrites from the trace.
lbcrypto::Ciphertext<DCRTPoly>
synthesize_haze_mrp_ciphertext(const Context &ctx, const std::vector<uint64_t> &moduli,
                               const std::vector<std::vector<uint64_t>> &per_residue_values) {
    auto ct = empty_ct_shell(ctx, /*k_count=*/1);
    auto &dcrt = ct->GetElements()[0];
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

// SRPArray/MRPArray: K-element CT; rejects heterogeneous per-element bases
// (would need multi-CC stitching OpenFHE doesn't support).
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
// per_element_moduli sizes as boundaries.
std::vector<std::vector<std::vector<uint64_t>>>
slice_array_values(const std::vector<std::vector<uint64_t>> &per_residue_values,
                   const std::vector<std::vector<uint64_t>> &per_element_moduli) {
    std::size_t expected = 0;
    for (const auto &m : per_element_moduli)
        expected += m.size();
    if (per_residue_values.size() != expected) {
        std::ostringstream body;
        body << "slice_array_values: per_residue_values.size()=" << per_residue_values.size()
             << " != sum of per_element_moduli sizes (" << expected << ")";
        throw std::runtime_error(body.str());
    }
    std::vector<std::vector<std::vector<uint64_t>>> out;
    out.reserve(per_element_moduli.size());
    size_t cursor = 0;
    for (const auto &elem_moduli : per_element_moduli) {
        std::vector<std::vector<uint64_t>> row;
        row.reserve(elem_moduli.size());
        for (size_t i = 0; i < elem_moduli.size(); ++i) {
            row.push_back(per_residue_values[cursor]);
            ++cursor;
        }
        out.push_back(std::move(row));
    }
    return out;
}

// Dispatch on shape.kind; empty per_residue_values yields a template.
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

// State captured into the post-recording hook lambda by Init/Register;
// freed when the next install replaces it or compiler().reset() is called.
struct HookCtx {
    uint64_t ring_dim;
    Context primary;
    // Register* sets true → `primary` covers every shape, no per-shape build.
    bool primary_covers_all_shapes;
};

// Pick the CC for a shape; uses `local_cache` (alive only during one hook
// call) to dedupe per-shape builds. Heterogeneous arrays log + return nullptr.
// TODO(niobium-fhetch:mod-map-tracking): the modulus-0 fallback covers
// outputs whose address_modulus_map entry isn't registered before sync.
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
    // Mirrors the heterogeneity check in synthesize_haze_array_ciphertext.
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
                log_hook_error(body.str());
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
        log_hook_error(body.str());
        return nullptr;
    }
    auto [ins, _] = local_cache.emplace(moduli, std::move(*built));
    return &ins->second;
}

// LOAD-BEARING. Runs between stop_recording and reconstruct to produce the
// OpenFHE artifacts the replay path needs. local_cache is per-call.
void on_post_recording(const HookCtx &hctx) {
    std::map<std::vector<uint64_t>, Context> local_cache;

    // ---- Inputs ----
    niobium::detail::for_each_captured_input([&](const niobium::CapturedInputRecord &rec) {
        // Keys aren't shape-tagged yet (Tier 2 / FIDESlib).
        if (rec.name == "evalmult_key" || rec.name == "automorphism_key")
            return;
        try {
            const auto *ctx = context_for_shape(hctx, local_cache, rec.shape);
            if (ctx == nullptr) {
                log_hook_error("input '" + rec.name + "': no CC available for shape");
                return;
            }
            auto ct = synthesize_for_shape(*ctx, rec.shape, rec.per_residue_values);
            if (!niobium::detail::write_ciphertext_input_bin(rec.name, ct, rec.addr_ids)) {
                log_hook_error("write_ciphertext_input_bin failed for '" + rec.name + "'");
            }
        } catch (const std::exception &e) {
            log_hook_error("input bin synthesis for '" + rec.name + "' threw: " + e.what());
        }
    });

    niobium::detail::for_each_captured_output(
        [&](const std::string &name, const niobium::CapturedShape &shape) {
            try {
                const auto *ctx = context_for_shape(hctx, local_cache, shape);
                if (ctx == nullptr) {
                    log_hook_error("output '" + name + "': no CC available for shape");
                    return;
                }
                auto ct = synthesize_for_shape(*ctx, shape, /*per_residue_values=*/{});
                if (!niobium::detail::write_ciphertext_template(name, ct)) {
                    log_hook_error("write_ciphertext_template failed for '" + name + "'");
                }
            } catch (const std::exception &e) {
                log_hook_error("template synthesis for '" + name + "' threw: " + e.what());
            }
        });
}

// Install the post-recording hook, moving `hctx` into the lambda closure;
// clears the bootstrap-precompute hook haze never uses.
void install_post_recording_hook(HookCtx hctx) {
    niobium::compiler().set_auto_capture_at_stop(nullptr);
    niobium::compiler().set_post_recording_hook([hctx = std::move(hctx)]() noexcept {
        try {
            on_post_recording(hctx);
        } catch (const std::exception &e) {
            log_hook_error(std::string{"post_recording_hook threw: "} + e.what());
        } catch (...) {
            log_hook_error("post_recording_hook threw (unknown)");
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

        *picked_modulus = first_tower_modulus(built->cc);

        // Plant program_name so cryptocontext.dat lands under haze/ rather
        // than the "niobium_trace" default; libhaze's later init is idempotent.
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
    // Clear first so any early-return below still honors the "prior
    // failures don't leak forward" contract.
    hook_had_error_flag().store(false, std::memory_order_relaxed);

    // Disk-only cleanup so stale template/probe names from prior tests don't
    // pollute MRP lookups; the hook lambda is freed by compiler().reset().
    // program_dir is queried live — reset_all() runs us before compiler().reset().
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

extern "C" int hazeReplayBridgeTakeHookHadError() noexcept {
    return hook_had_error_flag().exchange(false, std::memory_order_relaxed) ? 1 : 0;
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

        const uint64_t ring_dim = element_params->GetRingDimension();

        // Plant program_name; see Init for rationale.
        niobium::compiler().set_program_info("haze", "", "");
        niobium::compiler().capture_crypto_context(cc);

        install_post_recording_hook(HookCtx{
            .ring_dim = ring_dim,
            .primary = Context{.ring_dim = ring_dim, .cc = cc},
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
// fhetch::result() overloads — read serialized_probes/<name>.ct back as
// fhetch types. Default visibility so libhaze resolves them out of the bridge.
// ============================================================================

#define NIOBIUM_FHETCH_RESULT_API __attribute__((visibility("default")))

namespace niobium::fhetch {

namespace {

// Convert OpenFHE's global ::Format to fhetch::Format.
inline Format openfhe_to_fhetch_format(::Format fmt) {
    return fmt == ::Format::COEFFICIENT ? Format::Coefficient : Format::Evaluation;
}

// Deserialize <program_dir>/serialized_probes/<name>.ct; nullptr on error.
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

// Pull one residue's coefficients out of a NativePoly.
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
