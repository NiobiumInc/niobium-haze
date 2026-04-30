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
#pragma once

#include <string_view>

namespace haze {

// Single sink for [haze]-prefixed runtime diagnostics. Replaces ad-hoc
// `std::cerr << "[haze] ...";` blocks across haze + replay_bridge so the
// prefix is consistent and call sites stay short.
//
// Output format: "[haze] <tag>: <body>\n". `tag` identifies the subsystem
// (e.g. "epoch", "replay_bridge"). Both arguments are printed verbatim;
// callers compose their own error text.
void log_error(std::string_view tag, std::string_view body) noexcept;

} // namespace haze
