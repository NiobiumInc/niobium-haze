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
#include "core/epoch.hpp"

#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "common/errors.hpp"
#include "common/handle.hpp"
#include "core/polynomial_io.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>
#include <iostream>
#include <mutex>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <string>
#include <utility>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

EpochState &EpochState::instance() noexcept {
    static EpochState inst;
    return inst;
}

void EpochState::ensure_recording_locked() {
    // The session constructor calls backend().ensure_initialized() *before*
    // taking the epoch lock, so by the time we get here the backend is
    // already up. Defensively check is_initialized() — if the caller
    // somehow reached us without that prelude (e.g. a future code path
    // that bypasses EpochSession), staying out of the recording state
    // makes the failure visible at the next lookup_or_create rather than
    // crashing inside niobium::compiler().
    if (!backend().is_initialized())
        return;
    if (!recording_) {
        // start_epoch() before start(): first call memorizes the
        // polynomial-ID base; later calls (after stop_epoch in
        // do_materialize) reset the counter to that base. Without it,
        // poly IDs drift forward across materializations and each epoch's
        // compaction sees a different layout.
        backend().start_epoch();
        backend().start_recording();
        recording_ = true;
    }
}

bool EpochState::is_recording() noexcept {
    HazeLockGuard lock(mutex_);
    return recording_;
}

void EpochState::invalidate(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    // RYANPR: Is erase walking all of these maps? It would seem nice to be able to just know which one to erase from (as in device addresses also had a tag of which map they were in). On refactoring, make sure not to overengineer such a map of device address to which map it is in.
    poly_map_.erase(addr);
    compute_results_.erase(addr);
    pending_outputs_.erase(addr);
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = config().ring_dim();
    auto components = allocator().extract_polynomial_components(addr, ring_dim);

    fhetch::Polynomial poly;
    if (components) {
        poly = fhetch::Polynomial::from_data(*components, ring_dim, fhetch::Format::Evaluation);
    } else if (components.error() == HazeInternalError::NoData) {
        // Address allocated but no bytes ever written. Construct a fresh
        // zero polynomial via fhetch — its shared_ptr<MRPImpl> storage
        // owns the data, so HAZE never fabricates zero bytes itself.
        // Matches the FIDESlib pattern of using SPECIAL limb buffers
        // before their explicit zero_out memset (per task-5 design).
        poly = fhetch::Polynomial::zeros(ring_dim);
    } else {
        return std::unexpected(components.error());
    }

    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);

    poly_map_.emplace(addr, poly);
    return poly;
}

void EpochState::store_locked(DevAddr addr, niobium::fhetch::Polynomial poly) noexcept {
    poly_map_.insert_or_assign(addr, std::move(poly));
    // Track this address as a compute result so replay_and_populate
    // tags it as a fhetch output before stop()/replay() run. Input
    // polys created via lookup_or_create_locked are excluded — tagging
    // them as fhetch outputs would confuse the niobium compiler.
    // RYANPR: Not all stores are compute results, no?
    compute_results_.insert(addr);
}

std::expected<void, HazeInternalError> EpochState::replay_and_populate() noexcept {
    HazeLockGuard lock(mutex_);
    if (!recording_) {
        return {}; // nothing to replay
    }

    // Tag every bound compute result as a fhetch output. Inputs
    // (poly_map_ entries created by lookup_or_create_locked) are
    // deliberately excluded — their shadow data is already authoritative.
    // RYANPR: Since we have an authoritative place for inputs, why do we not have an authorative place for outputs? Then we wouldn't have to scan for them.
    for (DevAddr result_addr : compute_results_) {
        if (pending_outputs_.find(result_addr) == pending_outputs_.end()) {
            const std::string name = "haze_out_" + std::to_string(output_counter_++);
            pending_outputs_.emplace(result_addr, name);
        }
    }

    // RYANPR: Seems like we could collapse this with the prior for loop.
    for (auto &[addr, name] : pending_outputs_) {
        auto it = poly_map_.find(addr);
        if (it == poly_map_.end())
            continue;
        fhetch::tag_output(name, it->second);
    }

    return do_materialize_locked();
}

std::expected<void, HazeInternalError> EpochState::do_materialize_locked() {
    if (!recording_) {
        return {};
    }

    // Step 1: write the per-epoch .fhetch trace and reset the recording
    // bookkeeping in libnbfhetch.
    const bool stop_ok = backend().stop_epoch();
    if (!stop_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendError,
                              "EpochState::replay_and_populate (stop_epoch)");
        return std::unexpected(HazeInternalError::BackendError);
    }

    // Step 2: dispatch replay. For target=="local" this is a no-op
    // success (the .fhetch trace is the only artifact). For non-local
    // targets it spawns nbcc_fhetch_replay via the FHETCH HTTP transport
    // (forwarder on PATH -> server -> compiler-side binary running the
    // OpenFHE simulator) and writes ciphertext probes back to disk.
    // RYANPR: We have a mode where we test against local but if we are only writing out the fhetch IR how are we actually populating the shadow buffer?
    const bool replay_ok = backend().replay();
    if (!replay_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendError,
                              "EpochState::replay_and_populate (replay)");
        return std::unexpected(HazeInternalError::BackendError);
    }

    // Step 3: populate the host shadow buffer for each output address
    // so subsequent hazeMemcpy(D2H) reads pick up the computed values.
    // fhetch::result(name, Polynomial&) reads
    // <program_dir>/serialized_probes/<name>.ct (written by the
    // compiler-side nbcc_fhetch_replay) and unwraps the first NativePoly
    // into a fhetch::Polynomial. extract_polynomial_values rehydrates
    // the values via fhetch::save_polynomial_json, and update_shadow
    // commits them to the device shadow buffer at `addr`.
    for (auto &[addr, name] : pending_outputs_) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::cerr << "[haze] result('" << name << "') unavailable; "
                      << "shadow at addr 0x" << std::hex << to_uintptr(addr) << std::dec
                      << " is stale\n";
            continue;
        }
        std::vector<uint64_t> values;
        if (!extract_polynomial_values(result_poly, name, values)) {
            // RYANPR: Since we are doing these error operations in the same way every time we should make an `error.{cpp,hpp}` that encapsulates how we write out things and then takes a string to do it. We could replace this with more detailed logging later. All strings seem to start with `[haze]`.
            std::cerr << "[haze] failed to extract values for output '" << name
                      << "' at addr 0x" << std::hex << to_uintptr(addr) << std::dec << '\n';
            continue;
        }
        std::vector<uint8_t> bytes(values.size() * sizeof(uint64_t));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        allocator().update_shadow(addr, bytes);
    }

    // Clear niobium::compiler()'s captured input/output state so the next
    // recording-replay cycle inside the same hazeDeviceReset window starts
    // from an empty slate. Without this, captured_inputs/outputs accumulate
    // across cycles — multi-cycle tests like
    // "multiple materializations: two independent D2H cycles" would see
    // stale data shipped to the compiler-side replay.
    // RYANPR: Why is this called so late? It would seem that for error cases we are not clearing the captured inputs and outputs, which could lead to a screwed up compiler state if we then tried to recover from a failed run and succeeded the next time (or ran a second computation that succeeded when the first did not).
    niobium::compiler().clear_captured_inputs();
    niobium::compiler().clear_captured_outputs();

    clear_state_locked();
    return {};
}

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    compute_results_.clear();
    pending_outputs_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
}

void EpochState::reset() noexcept {
    HazeLockGuard lock(mutex_);
    clear_state_locked();
}

HazeMutex &EpochSession::init_then_get_mutex() noexcept {
    // ensure_initialized() has its own atomic + init mutex; safe (and
    // important) to call before acquiring the epoch lock so first-call
    // compiler init doesn't block other epoch-lock acquirers. The
    // bool return is intentionally not propagated here — failure is
    // caught at ensure_recording_locked() via is_initialized(), which
    // refuses to start recording if init didn't take. Subsequent
    // compute calls then fail with a coherent error from
    // lookup_or_create_locked rather than crashing inside niobium.
    [[maybe_unused]] const bool _ = backend().ensure_initialized();
    return epoch().mutex_;
}

hazeError_t copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    // D2H is a plain shadow read. Recording finalization + replay live
    // in haze::epoch().replay_and_populate() (exposed as hazeReplay()),
    // which the user must call explicitly before reading post-compute
    // values back. This separation is intentional: replay incurs an
    // HTTP transport round-trip to the compiler-side nbcc_fhetch_replay,
    // which is a heavy operation that should be triggered explicitly,
    // not as a side-effect of a memcpy.
    return allocator().copy_to_host(dst, src, count);
}

} // namespace haze
