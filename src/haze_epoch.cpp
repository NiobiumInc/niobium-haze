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
#include "haze_epoch.hpp"

#include "haze_allocator.hpp"
#include "haze_backend.hpp"
#include "haze_config.hpp"
#include "haze_errors.hpp"
#include "haze_handle.hpp"
#include "haze_polynomial_io.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>
#include <iostream>
#include <mutex>
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
    if (!components) {
        return std::unexpected(components.error());
    }

    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::Polynomial poly =
        fhetch::Polynomial::from_data(*components, ring_dim, fhetch::Format::Evaluation);
    fhetch::tag_input(name, poly);

    poly_map_.emplace(addr, poly);
    return poly;
}

void EpochState::store_locked(DevAddr addr, niobium::fhetch::Polynomial poly) noexcept {
    poly_map_.insert_or_assign(addr, std::move(poly));
    // Track this address as a compute result so flush_for_d2h knows to
    // materialize it on the first flush (input polys, added via
    // lookup_or_create_locked, are excluded — tagging them as fhetch
    // outputs would confuse stop_epoch).
    compute_results_.insert(addr);
}

std::expected<void, HazeInternalError> EpochState::flush_for_d2h(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    if (!recording_) {
        return {}; // shadow already authoritative
    }
    if (poly_map_.find(addr) == poly_map_.end()) {
        return {}; // address has no compute output bound
    }

    // Tag every compute result as a pending output, not just `addr`.
    // do_materialize_locked() runs stop_epoch + clear_state, so any
    // result not captured here would lose its value to subsequent D2H
    // calls in the same epoch — they would early-return on !recording_
    // and read uninitialised shadow. Multi-output ops (hazeBasisConvert,
    // hazeModDown, hazeModUp) routinely produce several bound polys
    // per call and the caller D2Hs each in turn, so capturing the
    // whole live set on the first flush is the only way that pattern
    // works without HAZE_PERSIST_WRITES.
    //
    // Inputs (poly_map_ entries created via lookup_or_create_locked)
    // are deliberately excluded — their shadow is already authoritative
    // (set by H2D), and tagging them as fhetch outputs causes
    // stop_epoch to fail with BackendError.
    for (DevAddr result_addr : compute_results_) {
        if (pending_outputs_.find(result_addr) == pending_outputs_.end()) {
            const std::string name = "haze_out_" + std::to_string(output_counter_++);
            pending_outputs_.emplace(result_addr, name);
        }
    }

    return do_materialize_locked();
}

std::expected<void, HazeInternalError> EpochState::do_materialize_locked() {
    if (!recording_) {
        return {};
    }

    for (auto &[addr, name] : pending_outputs_) {
        auto it = poly_map_.find(addr);
        if (it == poly_map_.end())
            continue;
        fhetch::tag_output(name, it->second);
    }

    // stop_epoch() compiles, replays, resets compiler state, and restarts
    // recording. State is reset at end of this function regardless of
    // backend success so the next epoch starts fresh.
    const bool ok = backend().stop_epoch();

    if (ok) {
        for (auto &[addr, name] : pending_outputs_) {
            fhetch::Polynomial result_poly;
            if (!fhetch::result(name, result_poly)) {
                std::cerr << "[haze] failed to retrieve result for output '" << name
                          << "' at addr 0x" << std::hex << to_uintptr(addr) << std::dec << '\n';
                continue;
            }
            std::vector<uint64_t> values;
            if (!extract_polynomial_values(result_poly, name, values)) {
                std::cerr << "[haze] failed to extract values for output '" << name
                          << "' at addr 0x" << std::hex << to_uintptr(addr) << std::dec << '\n';
                continue;
            }
            std::vector<uint8_t> bytes(values.size() * sizeof(uint64_t));
            std::memcpy(bytes.data(), values.data(), bytes.size());
            allocator().update_shadow(addr, bytes);
        }
    }

    clear_state_locked();
    if (!ok) {
        record_internal_error(HazeInternalError::BackendError, "EpochState::do_materialize");
        return std::unexpected(HazeInternalError::BackendError);
    }
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

hazeError_t copy_to_host_with_flush(void *dst, DevAddr src, size_t count) noexcept {
    // Single locked operation: flush_for_d2h decides internally whether
    // any work is required, so callers don't need a separate predicate
    // check (which would race a concurrent invalidate).
    auto result = epoch().flush_for_d2h(src);
    if (!result)
        return to_public_error(result.error());
    return allocator().copy_to_host(dst, src, count);
}

} // namespace haze
