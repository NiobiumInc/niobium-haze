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
#include "common/handle.hpp"
#include "core/input_spill.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using haze::DevAddr;

namespace {

DevAddr addr(uint64_t n) {
    return DevAddr{0x4000000000ULL + (n * 0x8000)};
}

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
TEST_CASE("input spill: put/read/erase round-trips one address, overwrite allowed", "[unit]") {
    const fs::path dir{"input_spill_scratch_roundtrip"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());
    REQUIRE(fs::is_directory(dir));

    constexpr std::size_t ring_dim = 64;
    const DevAddr a = addr(0);
    REQUIRE_FALSE(store.has(a));

    const std::vector<uint64_t> first = pattern(0, ring_dim);
    auto put_first = store.put(a, std::vector<uint64_t>(first));
    REQUIRE(put_first.has_value());
    REQUIRE(store.has(a));

    std::vector<uint64_t> full(ring_dim, 0);
    auto read_full = store.read(a, std::as_writable_bytes(std::span{full}));
    REQUIRE(read_full.has_value());
    REQUIRE(full == first);

    // Overwrite: the same address, a different residue.
    const std::vector<uint64_t> second = pattern(1, ring_dim);
    auto put_second = store.put(a, std::vector<uint64_t>(second));
    REQUIRE(put_second.has_value());
    std::vector<uint64_t> after_overwrite(ring_dim, 0);
    auto read_after_overwrite = store.read(a, std::as_writable_bytes(std::span{after_overwrite}));
    REQUIRE(read_after_overwrite.has_value());
    REQUIRE(after_overwrite == second);

    auto erased = store.erase(a);
    REQUIRE(erased.has_value());
    REQUIRE_FALSE(store.has(a));
    REQUIRE_FALSE(fs::exists(dir / "addr_4000000000.spill"));

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: read serves full and partial reads", "[unit]") {
    const fs::path dir{"input_spill_scratch_read"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 32;
    const DevAddr a = addr(1);
    const std::vector<uint64_t> residue = pattern(0, ring_dim);
    auto put_result = store.put(a, std::vector<uint64_t>(residue));
    REQUIRE(put_result.has_value());

    std::vector<uint64_t> full(ring_dim, 0);
    REQUIRE(store.read(a, std::as_writable_bytes(std::span{full})).has_value());
    REQUIRE(full == residue);

    std::vector<uint64_t> half(ring_dim / 2, 0);
    REQUIRE(store.read(a, std::as_writable_bytes(std::span{half})).has_value());
    for (std::size_t i = 0; i < ring_dim / 2; ++i) {
        REQUIRE(half[i] == residue[i]);
    }

    // An empty destination span is misuse, not a legitimate zero-length read.
    std::vector<uint64_t> empty_buf;
    auto empty_span = store.read(a, std::as_writable_bytes(std::span{empty_buf}));
    REQUIRE(!empty_span.has_value());
    REQUIRE(empty_span.error() == haze::HazeInternalError::SpillIoFailed);

    // ring_dim is 32, so a 33-element destination is oversized.
    std::vector<uint64_t> oversized(ring_dim + 1, 0);
    auto bad_count = store.read(a, std::as_writable_bytes(std::span{oversized}));
    REQUIRE(!bad_count.has_value());
    REQUIRE(bad_count.error() == haze::HazeInternalError::SpillIoFailed);

    // An address never put() is SpillRecordMissing, not a zero-filled read.
    auto missing = store.read(addr(9), std::as_writable_bytes(std::span{full}));
    REQUIRE(!missing.has_value());
    REQUIRE(missing.error() == haze::HazeInternalError::SpillRecordMissing);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: put validates shape and activation", "[unit]") {
    const fs::path dir{"input_spill_scratch_shape"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;

    auto before_activate = store.put(addr(0), pattern(0, 4));
    REQUIRE(!before_activate.has_value());
    REQUIRE(before_activate.error() == haze::HazeInternalError::SpillIoFailed);

    REQUIRE(store.activate(dir).has_value());

    auto empty_residue = store.put(addr(0), {});
    REQUIRE(!empty_residue.has_value());
    REQUIRE(empty_residue.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: erase is a hard error for an address never put", "[unit]") {
    const fs::path dir{"input_spill_scratch_erase"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    auto erased = store.erase(addr(0));
    REQUIRE(!erased.has_value());
    REQUIRE(erased.error() == haze::HazeInternalError::SpillRecordMissing);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: bind_name / take_named reads addrs in order without erasing bytes",
          "[unit]") {
    const fs::path dir{"input_spill_scratch_named"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 16;
    const DevAddr a0 = addr(0);
    const DevAddr a1 = addr(1);
    const std::vector<uint64_t> r0 = pattern(0, ring_dim);
    const std::vector<uint64_t> r1 = pattern(1, ring_dim);
    REQUIRE(store.put(a0, std::vector<uint64_t>(r0)).has_value());
    REQUIRE(store.put(a1, std::vector<uint64_t>(r1)).has_value());

    auto bound = store.bind_name("group", {a0, a1});
    REQUIRE(bound.has_value());

    auto taken = store.take_named("group");
    REQUIRE(taken.has_value());
    REQUIRE(taken->size() == 2);
    REQUIRE((*taken)[0] == r0);
    REQUIRE((*taken)[1] == r1);

    // The name binding is consumed, but the addrs' bytes are not: take_named again
    // fails on the name, while the addrs are still there to read directly.
    auto second_take = store.take_named("group");
    REQUIRE(!second_take.has_value());
    REQUIRE(second_take.error() == haze::HazeInternalError::SpillRecordMissing);
    REQUIRE(store.has(a0));
    REQUIRE(store.has(a1));
    std::vector<uint64_t> reread(ring_dim, 0);
    REQUIRE(store.read(a0, std::as_writable_bytes(std::span{reread})).has_value());
    REQUIRE(reread == r0);

    // An unbound name is SpillRecordMissing too.
    auto unknown_name = store.take_named("no-such-group");
    REQUIRE(!unknown_name.has_value());
    REQUIRE(unknown_name.error() == haze::HazeInternalError::SpillRecordMissing);

    // bind_name snapshots immediately: a later overwrite/erase of the addr cannot
    // perturb what an already-bound name reads back.
    REQUIRE(store.put(a1, std::vector<uint64_t>(r0)).has_value());
    auto rebound = store.bind_name("group2", {a1});
    REQUIRE(rebound.has_value());
    REQUIRE(store.put(a1, std::vector<uint64_t>(r1)).has_value()); // overwrite after snapshot
    REQUIRE(store.erase(a1).has_value());                          // and even free it
    auto taken2 = store.take_named("group2");
    REQUIRE(taken2.has_value());
    REQUIRE(taken2->size() == 1);
    REQUIRE((*taken2)[0] == r0); // the snapshot at bind_name time, not the later overwrite

    // bind_name on an addr with no record fails immediately (nothing to snapshot).
    auto dangling = store.bind_name("dangling", {a0, a1});
    REQUIRE(!dangling.has_value());
    REQUIRE(dangling.error() == haze::HazeInternalError::SpillIoFailed);
    // a0's snapshot (index 0) was written before a1 (index 1) failed; the rollback must
    // remove it too, not just report the error.
    REQUIRE_FALSE(fs::exists(dir / "name_dangling_0.spill"));

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: snapshots are immune to a later put's rename", "[unit]") {
    const fs::path dir{"input_spill_scratch_fresh_inode"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 16;
    const DevAddr a = addr(0);
    const std::vector<uint64_t> original = pattern(0, ring_dim);
    REQUIRE(store.put(a, std::vector<uint64_t>(original)).has_value());
    REQUIRE(store.bind_name("snap", {a}).has_value());

    // put()'s write-then-rename always plants a fresh inode at addr's path, so the
    // hardlinked snapshot keeps the ORIGINAL bytes even after this overwrite.
    const std::vector<uint64_t> updated = pattern(1, ring_dim);
    REQUIRE(store.put(a, std::vector<uint64_t>(updated)).has_value());

    auto taken = store.take_named("snap");
    REQUIRE(taken.has_value());
    REQUIRE(taken->size() == 1);
    REQUIRE((*taken)[0] == original);

    std::vector<uint64_t> readback(ring_dim, 0);
    REQUIRE(store.read(a, std::as_writable_bytes(std::span{readback})).has_value());
    REQUIRE(readback == updated);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: activate is idempotent for the same root, a hard error for a different one",
          "[unit]") {
    const fs::path dir_a{"input_spill_scratch_root_a"};
    const fs::path dir_b{"input_spill_scratch_root_b"};
    std::error_code ec;
    fs::remove_all(dir_a, ec);
    fs::remove_all(dir_b, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir_a).has_value());
    const DevAddr a = addr(0);
    REQUIRE(store.put(a, pattern(0, 8)).has_value());

    // Re-activating the SAME root while active keeps existing records.
    REQUIRE(store.activate(dir_a).has_value());
    REQUIRE(store.has(a));

    // A DIFFERENT root while active is refused outright.
    auto different_root = store.activate(dir_b);
    REQUIRE(!different_root.has_value());
    REQUIRE(different_root.error() == haze::HazeInternalError::SpillIoFailed);
    REQUIRE_FALSE(fs::exists(dir_b));
    REQUIRE(store.has(a));

    fs::remove_all(dir_a, ec);
    fs::remove_all(dir_b, ec);
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

    // A failed activate leaves the store inactive: put() behaves as if
    // activate() had never been called.
    auto put_after_failed_activate = store.put(addr(0), pattern(0, 4));
    REQUIRE(!put_after_failed_activate.has_value());
    REQUIRE(put_after_failed_activate.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove(blocker, ec);
}

TEST_CASE("input spill: clear_names drops manifests only; clear tears everything down", "[unit]") {
    const fs::path dir{"input_spill_scratch_clear"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    const DevAddr a = addr(0);
    const DevAddr b = addr(1);
    REQUIRE(store.put(a, pattern(0, 8)).has_value());
    REQUIRE(store.put(b, pattern(1, 8)).has_value());
    REQUIRE(store.bind_name("g", {a, b}).has_value());

    store.clear_names();
    // clear_names() drops the manifest, not the bytes: the addrs are still there.
    REQUIRE(store.has(a));
    REQUIRE(store.has(b));
    auto after_clear_names = store.take_named("g");
    REQUIRE(!after_clear_names.has_value());
    REQUIRE(after_clear_names.error() == haze::HazeInternalError::SpillRecordMissing);

    store.clear();
    REQUIRE(!fs::exists(dir));
    REQUIRE_FALSE(store.has(a));

    auto put_after_clear = store.put(a, pattern(0, 8));
    REQUIRE(!put_after_clear.has_value());
    REQUIRE(put_after_clear.error() == haze::HazeInternalError::SpillIoFailed);

    REQUIRE(store.activate(dir).has_value());
    const std::vector<uint64_t> expected = pattern(2, 8);
    REQUIRE(store.put(a, std::vector<uint64_t>(expected)).has_value());
    std::vector<uint64_t> readback(8, 0);
    REQUIRE(store.read(a, std::as_writable_bytes(std::span{readback})).has_value());
    REQUIRE(readback == expected);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: take_named terminalizes when snapshot deletion fails", "[unit]") {
    const fs::path dir{"input_spill_scratch_terminal"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 8;
    const DevAddr a0 = addr(0);
    const DevAddr a1 = addr(1);
    REQUIRE(store.put(a0, pattern(0, ring_dim)).has_value());
    REQUIRE(store.put(a1, pattern(1, ring_dim)).has_value());
    REQUIRE(store.bind_name("g", {a0, a1}).has_value());

    // Block deletion, not reading, of the name-scoped snapshots: owner_read | owner_exec
    // permits open()/read() but not unlink(), which needs write on the containing dir.
    // No REQUIRE runs between the chmod and its restore, so a failed assertion can never
    // leave the directory unwritable for this case's own cleanup or a later case.
    const fs::perms original = fs::status(dir).permissions();
    // error_code overloads throughout: a throw here (instead of a returned error) would
    // skip the restore below and strand the directory at 0500 for later cases.
    std::error_code restrict_ec;
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace,
                    restrict_ec);
    auto first_take = store.take_named("g");
    std::error_code restore_ec;
    fs::permissions(dir, original, fs::perm_options::replace, restore_ec);
    REQUIRE_FALSE(restrict_ec);
    REQUIRE_FALSE(restore_ec);
    REQUIRE(!first_take.has_value());
    REQUIRE(first_take.error() == haze::HazeInternalError::SpillIoFailed);

    // The record is retired even though its data could not be withdrawn cleanly: a
    // retry sees an honest SpillRecordMissing, never a half-deleted record read back
    // as if it were still intact.
    auto second_take = store.take_named("g");
    REQUIRE(!second_take.has_value());
    REQUIRE(second_take.error() == haze::HazeInternalError::SpillRecordMissing);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: bind_name and take_named reject a path-separator name", "[unit]") {
    const fs::path dir{"input_spill_scratch_name_validation"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    const DevAddr a = addr(0);
    REQUIRE(store.put(a, pattern(0, 8)).has_value());

    auto bad_bind = store.bind_name("nested/name", {a});
    REQUIRE(!bad_bind.has_value());
    REQUIRE(bad_bind.error() == haze::HazeInternalError::SpillIoFailed);
    REQUIRE_FALSE(fs::exists(dir / "nested"));

    auto bad_take = store.take_named("nested/name");
    REQUIRE(!bad_take.has_value());
    REQUIRE(bad_take.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: bind_name re-registration with fewer addrs drops the trailing snapshot",
          "[unit]") {
    const fs::path dir{"input_spill_scratch_shrink"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 8;
    const DevAddr a0 = addr(0);
    const DevAddr a1 = addr(1);
    REQUIRE(store.put(a0, pattern(0, ring_dim)).has_value());
    REQUIRE(store.put(a1, pattern(1, ring_dim)).has_value());

    REQUIRE(store.bind_name("g", {a0, a1}).has_value());
    REQUIRE(fs::exists(dir / "name_g_0.spill"));
    REQUIRE(fs::exists(dir / "name_g_1.spill"));

    // Re-register the same name with one fewer addr: the trailing snapshot must be
    // removed from disk, not merely dropped from the in-memory record.
    REQUIRE(store.bind_name("g", {a0}).has_value());
    REQUIRE(fs::exists(dir / "name_g_0.spill"));
    REQUIRE_FALSE(fs::exists(dir / "name_g_1.spill"));

    auto taken = store.take_named("g");
    REQUIRE(taken.has_value());
    REQUIRE(taken->size() == 1);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: take_named rejects a corrupted record file", "[unit]") {
    const fs::path dir{"input_spill_scratch_corrupt"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 16;
    const DevAddr a = addr(0);
    REQUIRE(store.put(a, pattern(0, ring_dim)).has_value());
    REQUIRE(store.bind_name("corrupt", {a}).has_value());

    // take_named reads the name-scoped snapshot bind_name wrote, not the addr-scoped
    // file (already snapshotted and independent of it), so corrupt that one.
    {
        std::fstream f(dir / "name_corrupt_0.spill",
                       std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f.is_open());
        const char garbage[4] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.seekp(0, std::ios::beg);
        f.write(garbage, sizeof(garbage));
    }

    auto first_take = store.take_named("corrupt");
    REQUIRE(!first_take.has_value());
    REQUIRE(first_take.error() == haze::HazeInternalError::SpillIoFailed);

    // The name binding is retained on failure: a second attempt fails the same way
    // rather than reporting the name as missing.
    auto second_take = store.take_named("corrupt");
    REQUIRE(!second_take.has_value());
    REQUIRE(second_take.error() == haze::HazeInternalError::SpillIoFailed);

    fs::remove_all(dir, ec);
}

TEST_CASE("input spill: put fails when its scratch path is blocked by a directory, "
          "and cleans it up",
          "[unit]") {
    // Mechanism check for a deterministic per-address put() failure (used by the MRP
    // prefix-rollback integration test): put()'s scratch path is dir/addr_<hex>.spill.tmp,
    // and an ofstream cannot open an existing directory for writing, so pre-creating one
    // there fails ONLY this address's put(), leaving every other address unaffected.
    const fs::path dir{"input_spill_scratch_tmp_blocked"};
    std::error_code ec;
    fs::remove_all(dir, ec);

    haze::InputSpillStore store;
    REQUIRE(store.activate(dir).has_value());

    constexpr std::size_t ring_dim = 8;
    const DevAddr a = addr(0);
    std::ostringstream tmp_name;
    tmp_name << "addr_" << std::hex << haze::to_uintptr(a) << ".spill.tmp";
    const fs::path tmp_path = dir / tmp_name.str();
    REQUIRE(fs::create_directory(tmp_path));

    auto blocked = store.put(a, pattern(0, ring_dim));
    REQUIRE(!blocked.has_value());
    REQUIRE(blocked.error() == haze::HazeInternalError::SpillIoFailed);
    REQUIRE_FALSE(store.has(a));
    // write_residue_file's own failure cleanup removes the path it just failed to open
    // (an empty directory), so the blocker is already gone -- nothing to clear by hand.
    REQUIRE_FALSE(fs::exists(tmp_path));

    auto retried = store.put(a, pattern(0, ring_dim));
    REQUIRE(retried.has_value());
    REQUIRE(store.has(a));

    fs::remove_all(dir, ec);
}
