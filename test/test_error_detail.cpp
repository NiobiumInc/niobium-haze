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
// White-box unit tests for the error/detail plumbing in common/errors.hpp
// and common/log.hpp: the 1:1 pairing between g_last_error and
// g_last_error_message that set_error establishes, the append-not-overwrite
// pending slot, the warning-only sink that keeps non-fatal diagnostics out of
// it, hazeDeviceReset's clearing contract, and thread-locality. These test
// the internal machinery directly — haze_tests links haze_objs, not just the
// public C ABI — because the properties belong to that machinery's own
// contract, independent of which public entry point happens to exercise it.

#include "common/errors.hpp"
#include "common/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <string>
#include <thread>

namespace {

// Clears g_last_error, g_last_error_message and g_error_detail, the same way
// hazeDeviceReset() clears them for a real caller — run at the top of every
// case so run order can't leak state between them.
void reset_error_state() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

} // namespace

TEST_CASE("append_error_detail joins multiple causes, innermost first", "[unit]") {
    reset_error_state();
    // Simulates a chained failure: an inner diagnosis (e.g. fhetch::result's
    // load failure) followed by the outer context that gives up on it (e.g.
    // materialize's BackendOutputMissing) — both record_internal_error, so
    // both append.
    haze::record_internal_error(haze::HazeInternalError::SpillIoFailed, "inner cause");
    haze::record_internal_error(haze::HazeInternalError::BridgeHookFailed, "outer context");
    REQUIRE(set_error(HAZE_ERROR_INTERNAL) == HAZE_ERROR_INTERNAL);

    const std::string message = g_last_error_message;
    const auto inner = message.find("inner cause");
    const auto outer = message.find("outer context");
    REQUIRE(inner != std::string::npos);
    REQUIRE(outer != std::string::npos);
    REQUIRE(inner < outer);
    REQUIRE(message.find("; ") != std::string::npos);
}

TEST_CASE("record_internal_warning and log_warning never enter the pending slot", "[unit]") {
    reset_error_state();
    haze::record_internal_warning(haze::HazeInternalError::SpillIoFailed, "diagnostic only");
    haze::log_warning("test", "also diagnostic only");

    // A successful call after the warnings above must not surface either of
    // them: nothing was appended, so set_error's exchange yields "".
    REQUIRE(set_error(HAZE_SUCCESS) == HAZE_SUCCESS);
    REQUIRE(g_last_error_message.empty());
}

TEST_CASE("a plain failure after a detailed one carries no detail", "[unit]") {
    reset_error_state();
    haze::record_internal_error(haze::HazeInternalError::SpillIoFailed, "detailed cause");
    REQUIRE(set_error(HAZE_ERROR_INTERNAL) == HAZE_ERROR_INTERNAL);
    // Deliberately not read back here: the point is that set_error's own
    // exchange — not a prior read — is what keeps the next failure clean.

    // A later, unrelated plain failure (no record_internal_error/log_error
    // call anywhere in its path) must carry "", not the previous failure's
    // text.
    REQUIRE(set_error(HAZE_ERROR_INVALID_VALUE) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(g_last_error_message.empty());
}

TEST_CASE("hazeDeviceReset clears the last error, its message, and any pending detail", "[unit]") {
    reset_error_state();
    haze::record_internal_error(haze::HazeInternalError::SpillIoFailed, "about to be reset");
    REQUIRE(set_error(HAZE_ERROR_INTERNAL) == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    REQUIRE(g_last_error_message.empty());

    // A warning logged just before a reset (nothing ever committed it into
    // g_last_error_message) must not surface afterward either.
    haze::record_internal_warning(haze::HazeInternalError::SpillIoFailed, "warning before reset");
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(g_last_error_message.empty());
}

TEST_CASE("the last-error message is thread-local", "[unit]") {
    reset_error_state();
    haze::record_internal_error(haze::HazeInternalError::SpillIoFailed, "main thread detail");
    REQUIRE(set_error(HAZE_ERROR_INTERNAL) == HAZE_ERROR_INTERNAL);

    std::string other_thread_message = "not yet set";
    std::thread t([&] { other_thread_message = g_last_error_message; });
    t.join();
    // A fresh thread has recorded nothing of its own, regardless of what the
    // main thread just did.
    REQUIRE(other_thread_message.empty());

    // The main thread's own message survived the other thread's read.
    const std::string message = g_last_error_message;
    REQUIRE(message.find("main thread detail") != std::string::npos);
}
