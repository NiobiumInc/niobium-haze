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

// Tagged sink for [haze]-prefixed runtime diagnostics from replay_bridge (libhaze
// internal failures route through record_internal_error instead); prints
// "[haze] <tag>: <body>\n" with both arguments verbatim.
void log_error(std::string_view tag, std::string_view body) noexcept;

} // namespace haze
