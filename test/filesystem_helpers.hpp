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
// Filesystem helpers for test cases: a unique scratch directory and a scope
// guard for the working directory. Depends on Catch2 and the standard library
// only, so unit and integration tests alike can include it.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h> // IWYU pragma: keep -- mkdtemp; clang-tidy's include-cleaner has no mapping for it
#include <vector>

namespace haze::test {

// Hermetic scratch directory unique to this process (mkdtemp's random
// suffix): the host is shared with other users and runs, so a fixed name
// under fs::temp_directory_path() would collide. Created with mkdtemp's 0700
// permissions.
inline std::filesystem::path make_unique_temp_dir(std::string_view prefix) {
    std::string templ =
        (std::filesystem::temp_directory_path() / (std::string(prefix) + "_XXXXXX")).string();
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    // mkdtemp has no header mapping in clang-tidy's include-cleaner database;
    // <unistd.h> above is the real, correct provider (see its IWYU comment).
    // NOLINTNEXTLINE(misc-include-cleaner)
    REQUIRE(mkdtemp(buf.data()) != nullptr);
    return {buf.data()};
}

// Restores the process's working directory on scope exit, so a test that
// changes it cannot leak that change into whichever test Catch2 runs next in
// this same process. Runs even if a REQUIRE/CHECK fails: Catch2 reports a
// failure as a thrown exception that unwinds the stack normally. A failed
// restore aborts the process, since every later test would run elsewhere.
class RestoreCwd {
  public:
    RestoreCwd() : original_(std::filesystem::current_path()) {}
    ~RestoreCwd() {
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
        if (ec) {
            std::cerr << "RestoreCwd: failed to restore the working directory: "
                      << original_.string() << " (" << ec.message() << ")\n";
            std::abort();
        }
    }
    RestoreCwd(const RestoreCwd &) = delete;
    RestoreCwd &operator=(const RestoreCwd &) = delete;

  private:
    std::filesystem::path original_;
};

} // namespace haze::test
