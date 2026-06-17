// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
//
// C++ companion to replay_bridge.h (which is the pure-C ABI). Declares the
// bridge-provided niobium::fhetch readback helpers that are NOT part of
// fhetch's canonical fhetch_api.h spec.
//
// result_from() is the directory-explicit, singleton-free reader the isolated
// (concurrent) replay path uses: it reads <dir>/serialized_probes/<name>.ct
// directly, consulting no Compiler-singleton state, so N replays into distinct
// dirs can be collected concurrently. It is defined in the bridge
// (openfhe_template.cpp) alongside result(), and consumed by core/lower.cpp.
// It lives here, consumer-side, rather than in fhetch_api.h: fhetch_api.h is
// the FHETCH IR spec kept in sync with niobium-compiler, and result_from is a
// haze-only helper, so its declaration and definition both stay in haze.

#ifndef HAZE_REPLAY_BRIDGE_HPP
#define HAZE_REPLAY_BRIDGE_HPP

#include <filesystem>
#include <niobium/fhetch_api.h> // niobium::fhetch::{Polynomial, MRP}
#include <string>

namespace niobium::fhetch {

/// Directory-explicit, singleton-free variants of result(): read
/// <dir>/serialized_probes/<name>.ct directly, so concurrent replays into
/// distinct dirs can be collected without racing on the Compiler singleton's
/// program directory. result(name, ...) == result_from(<program_dir>, name, ...).
bool result_from(const std::filesystem::path &dir, const std::string &name, Polynomial &p);
bool result_from(const std::filesystem::path &dir, const std::string &name, MRP &m);

} // namespace niobium::fhetch

#endif // HAZE_REPLAY_BRIDGE_HPP
