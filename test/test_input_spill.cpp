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
#include "common/errors.hpp"
#include "core/input_spill.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint64_t> pattern(std::size_t residue_idx, std::size_t ring_dim) {
    std::vector<uint64_t> values(ring_dim);
    for (std::size_t i = 0; i < ring_dim; ++i) {
        values[i] = (residue_idx * 1000) + i;
    }
    return values;
}

} // namespace

// One local store per case (never the process-wide singleton), and one scratch
// dir per case so cases cannot see each other's on-disk state.
TEST_CASE("input spill: activate creates the dir and round-trips a record", "[unit]") {
    const fs::path dir{"input_spill_scratch_roundtrip"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());
    REQUIRE(fs::is_directory(dir));

    constexpr std::size_t ring_dim = 64;
    std::vector<std::vector<uint64_t>> residues;
    residues.reserve(3);
    for (std::size_t i = 0; i < 3; ++i) {
        residues.push_back(pattern(i, ring_dim));
    }
    const std::vector<std::vector<uint64_t>> expected = residues;

    auto put_result = store.put("in0", std::move(residues));
    REQUIRE(put_result.has_value());

    auto taken = store.take("in0");
    REQUIRE(taken.has_value());
    REQUIRE(*taken == expected);

    // take() is the flush-time consumer: it erases the file, not just the record.
    REQUIRE(!fs::exists(dir / "in0.spill"));

    auto second = store.take("in0");
    REQUIRE(!second.has_value());
    REQUIRE(second.error() == haze::HazeInternalError::SpillRecordMissing);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: read_residue serves full and partial reads", "[unit]") {
    const fs::path dir{"input_spill_scratch_read"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 32;
    const std::vector<uint64_t> residue0 = pattern(0, ring_dim);
    const std::vector<uint64_t> residue1 = pattern(1, ring_dim);
    std::vector<std::vector<uint64_t>> residues{residue0, residue1};
    auto put_result = store.put("in1", std::move(residues));
    REQUIRE(put_result.has_value());

    std::vector<uint64_t> full(ring_dim, 0);
    REQUIRE(store.read_residue("in1", 0, std::as_writable_bytes(std::span{full})).has_value());
    REQUIRE(full == residue0);

    std::vector<uint64_t> half(ring_dim / 2, 0);
    REQUIRE(store.read_residue("in1", 1, std::as_writable_bytes(std::span{half})).has_value());
    for (std::size_t i = 0; i < ring_dim / 2; ++i) {
        REQUIRE(half[i] == residue1[i]);
    }

    // An empty destination span is misuse, not a legitimate zero-length read.
    std::vector<uint64_t> empty_buf;
    auto empty_span = store.read_residue("in1", 0, std::as_writable_bytes(std::span{empty_buf}));
    REQUIRE(!empty_span.has_value());
    REQUIRE(empty_span.error() == haze::HazeInternalError::SpillIoFailed);

    // residue_count is 2, so index 2 is out of range.
    auto bad_idx = store.read_residue("in1", 2, std::as_writable_bytes(std::span{full}));
    REQUIRE(!bad_idx.has_value());
    REQUIRE(bad_idx.error() == haze::HazeInternalError::SpillIoFailed);

    std::vector<uint64_t> oversized(ring_dim + 1, 0);
    auto bad_count = store.read_residue("in1", 0, std::as_writable_bytes(std::span{oversized}));
    REQUIRE(!bad_count.has_value());
    REQUIRE(bad_count.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: put validates shape and activation", "[unit]") {
    const fs::path dir{"input_spill_scratch_shape"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;

    std::vector<std::vector<uint64_t>> one_residue{pattern(0, 4)};
    auto before_activate = store.put("x", std::move(one_residue));
    REQUIRE(!before_activate.has_value());
    REQUIRE(before_activate.error() == haze::HazeInternalError::SpillIoFailed);

    REQUIRE(store.activate(dir).has_value());

    auto empty_vec = store.put("x", {});
    REQUIRE(!empty_vec.has_value());
    REQUIRE(empty_vec.error() == haze::HazeInternalError::SpillIoFailed);

    std::vector<std::vector<uint64_t>> unequal{{1, 2, 3}, {1, 2}};
    auto unequal_result = store.put("x", std::move(unequal));
    REQUIRE(!unequal_result.has_value());
    REQUIRE(unequal_result.error() == haze::HazeInternalError::SpillIoFailed);

    std::vector<std::vector<uint64_t>> first{pattern(0, 4)};
    auto first_put = store.put("dup", std::move(first));
    REQUIRE(first_put.has_value());
    std::vector<std::vector<uint64_t>> second{pattern(9, 4)};
    const std::vector<std::vector<uint64_t>> expected_second = second;
    auto second_put = store.put("dup", std::move(second));
    REQUIRE(second_put.has_value());

    auto taken = store.take("dup");
    REQUIRE(taken.has_value());
    REQUIRE(*taken == expected_second);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: activate failure is a hard error", "[unit]") {
    const fs::path blocker{"input_spill_scratch_blocker"};
    std::error_code ec;
    fs::remove_all(blocker, ec);
    {
        std::ofstream f(blocker);
        f << "not a directory";
    }

    haze::InputSpillStore store;
    auto result = store.activate(blocker / "nested");
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == haze::HazeInternalError::SpillIoFailed);
    REQUIRE(!store.active());

    fs::remove(blocker, ec);
}

TEST_CASE("input spill: clear removes files, dir and deactivates", "[unit]") {
    const fs::path dir{"input_spill_scratch_clear"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    std::vector<std::vector<uint64_t>> r0{pattern(0, 8)};
    std::vector<std::vector<uint64_t>> r1{pattern(1, 8)};
    auto put_a = store.put("a", std::move(r0));
    REQUIRE(put_a.has_value());
    auto put_b = store.put("b", std::move(r1));
    REQUIRE(put_b.has_value());

    store.clear();

    REQUIRE(!fs::exists(dir));
    REQUIRE(!store.active());

    auto put_after_clear = store.put("a", {pattern(0, 8)});
    REQUIRE(!put_after_clear.has_value());
    REQUIRE(put_after_clear.error() == haze::HazeInternalError::SpillIoFailed);

    REQUIRE(store.activate(dir).has_value());
    std::vector<std::vector<uint64_t>> r2{pattern(2, 8)};
    const std::vector<std::vector<uint64_t>> expected = r2;
    auto put_c = store.put("c", std::move(r2));
    REQUIRE(put_c.has_value());
    auto taken = store.take("c");
    REQUIRE(taken.has_value());
    REQUIRE(*taken == expected);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: take rejects a corrupted record file", "[unit]") {
    const fs::path dir{"input_spill_scratch_corrupt"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 16;
    std::vector<std::vector<uint64_t>> residues{pattern(0, ring_dim), pattern(1, ring_dim)};
    auto put_result = store.put("corrupt", std::move(residues));
    REQUIRE(put_result.has_value());

    {
        std::fstream f(dir / "corrupt.spill", std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f.is_open());
        const char garbage[4] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.seekp(0, std::ios::beg);
        f.write(garbage, sizeof(garbage));
    }

    auto first_take = store.take("corrupt");
    REQUIRE(!first_take.has_value());
    REQUIRE(first_take.error() == haze::HazeInternalError::SpillIoFailed);

    // The record is retained on failure: a second take fails the same way rather than
    // reporting the record as missing.
    auto second_take = store.take("corrupt");
    REQUIRE(!second_take.has_value());
    REQUIRE(second_take.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove_all(dir, ec);
}
