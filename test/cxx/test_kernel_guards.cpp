// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Compile-time guard tests for haze::cxx::kernel. Every static_assert
// here is a class of kernel-author bug (most likely an AI-generated
// one) that the typed layer turns from a silent runtime hazard into a
// compile error. The single TEST_CASE keeps Catch2 happy; the assertions
// fire at build time.

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <haze/cxx/haze.hpp>
#include <type_traits>
#include <vector>

namespace {

using haze::cxx::In;
using haze::cxx::InOut;
using haze::cxx::Mrp;
using haze::cxx::Out;
using haze::cxx::Srp;
using haze::cxx::Status;

// ---- accepted shapes ----
constexpr auto ok_status = [](In<Mrp> /*a*/, Out<Mrp> /*dst*/, uint64_t /*k*/) -> Status {
    return Status{};
};
constexpr auto ok_void = [](In<Srp> /*a*/, InOut<Srp> /*dst*/) {};
// By-value memo keys are the tested shape, not a perf bug.
// NOLINTBEGIN(performance-unnecessary-value-param)
constexpr auto ok_keys = [](Out<Mrp> /*dst*/, haze::cxx::ModSlot /*m*/,
                            std::vector<uint64_t> /*scalars*/) -> Status { return Status{}; };
// NOLINTEND(performance-unnecessary-value-param)
static_assert(haze::cxx::kernel_compatible<decltype(ok_status)>);
static_assert(haze::cxx::kernel_compatible<decltype(ok_void)>);
static_assert(haze::cxx::kernel_compatible<decltype(ok_keys)>);

// ---- rejected shapes ----

// kernel() acceptance probed through a dependent context (a bare
// requires-expression at namespace scope has no SFINAE and would
// hard-error on the deleted catch-all).
template <class F>
concept kernel_constructible = requires(F body) { haze::cxx::kernel("probe", body); };

// Generic lambda: the signature is the kernel's interface; auto params
// make it un-introspectable.
constexpr auto generic = [](auto /*a*/) {};
static_assert(!haze::cxx::kernel_compatible<decltype(generic)>);
static_assert(!kernel_constructible<decltype(generic)>);

// Mutable lambda: non-const operator() is mutable state by definition.
[[maybe_unused]] const auto mutable_body = [](In<Srp> /*a*/) mutable {};
static_assert(!haze::cxx::kernel_compatible<std::remove_cvref_t<decltype(mutable_body)>>);

// Bare buffer parameter: buffers must declare a role (In/Out/InOut) —
// it is what drives output tagging and, later, replay binding.
constexpr auto bare_buffer = [](Mrp & /*a*/) {};
static_assert(!haze::cxx::kernel_compatible<decltype(bare_buffer)>);

// Unhashable value parameter: every non-buffer argument is a memo key
// and must satisfy param_hash.
// NOLINTNEXTLINE(performance-unnecessary-value-param) — the by-value param is the point
constexpr auto unhashable = [](In<Mrp> /*a*/, std::vector<float> /*w*/) {};
static_assert(!haze::cxx::kernel_compatible<decltype(unhashable)>);

// Buffer-returning body: a replayed call skips the body, so outputs
// must be caller-pre-allocated Out<> parameters, never return values.
constexpr auto returns_value = [](In<Srp> /*a*/) -> double { return 1.5; };
static_assert(!haze::cxx::kernel_compatible<decltype(returns_value)>);

// ---- call-site binding ----
constexpr auto needs_out =
    haze::cxx::kernel("needs_out", [](Out<Mrp> /*dst*/) -> Status { return Status{}; });
// Out<> requires a mutable lvalue: const handles and temporaries are
// compile errors, not runtime surprises.
static_assert(std::invocable<decltype(needs_out), Mrp &>);
static_assert(!std::invocable<decltype(needs_out), const Mrp &>);
static_assert(!std::invocable<decltype(needs_out), Mrp &&>);
// Arity is part of the interface.
static_assert(!std::invocable<decltype(needs_out), Mrp &, Mrp &>);

constexpr auto takes_key = haze::cxx::kernel(
    "takes_key", [](In<Mrp> /*a*/, uint64_t /*k*/) -> Status { return Status{}; });
static_assert(std::invocable<decltype(takes_key), const Mrp &, int>); // key converts
static_assert(!std::invocable<decltype(takes_key), const Mrp &, const char *>);

} // namespace

TEST_CASE("haze::cxx kernel guards hold at compile time", "[unit][cxx]") {
    // Capturing lambda: hidden state a memoized replay would go stale
    // on. Function scope — captures need automatic storage to exist.
    const int captured = 7;
    const auto capturing = [captured](In<Srp> /*a*/) { (void)captured; };
    static_assert(!haze::cxx::kernel_compatible<std::remove_cvref_t<decltype(capturing)>>);
    static_assert(!kernel_constructible<std::remove_cvref_t<decltype(capturing)>>);

    // The assertions above and at namespace scope are the test; this
    // anchors them in the run.
    REQUIRE(haze::cxx::kernel_compatible<decltype(ok_status)>);
}
