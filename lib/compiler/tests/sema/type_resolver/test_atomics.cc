#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {
    constexpr std::string_view PTR_PRELUDE{"var x: i32 = 0; var p: ^mut i32 = ^mut x; "};
} // namespace

TEST_CASE("@atomicLoad resolves to T") {
    auto [ctx, idx]{helpers::resolve_and_check(fmt::format(
        "{}const r := @atomicLoad(i32, p, builtin::MemoryOrder.seq_cst);", PTR_PRELUDE))};
    const auto [sym, data, type]{ctx->get_type_sym_info<sema::symbols::node_t>("r", idx)};
    CHECK(type == ctx->get_int_type(32, true));
}

TEST_CASE("@atomicStore resolves to void") {
    auto [ctx, idx]{helpers::resolve_and_check(fmt::format(
        "{}const r := @atomicStore(p, 1, builtin::MemoryOrder.seq_cst);", PTR_PRELUDE))};
    const auto [sym, data, type]{ctx->get_type_sym_info<sema::symbols::node_t>("r", idx)};
    CHECK(type == ctx->get_type(sema::type_kind::VOID_));
}

TEST_CASE("@atomicRmw resolves to T") {
    auto [ctx, idx]{helpers::resolve_and_check(
        fmt::format("{}const r := @atomicRmw(i32, p, builtin::AtomicRmwOp.add, 1, "
                    "builtin::MemoryOrder.seq_cst);",
                    PTR_PRELUDE))};
    const auto [sym, data, type]{ctx->get_type_sym_info<sema::symbols::node_t>("r", idx)};
    CHECK(type == ctx->get_int_type(32, true));
}

TEST_CASE("@cmpxchgWeak / @cmpxchgStrong resolve to bool") {
    const auto src{std::string{PTR_PRELUDE} +
                   "var out: i32 = 0; const r := @cmpxchgWeak(i32, p, "
                   "0, 1, builtin::MemoryOrder.seq_cst, builtin::MemoryOrder.relaxed, "
                   "&mut out);"};
    auto [ctx, idx]{helpers::resolve_and_check(src)};
    const auto [sym, data, type]{ctx->get_type_sym_info<sema::symbols::node_t>("r", idx)};
    CHECK(type == ctx->get_type(sema::type_kind::BOOL));
}

TEST_CASE("@fence resolves to void") {
    auto [ctx, idx]{helpers::resolve_and_check("const r := @fence(builtin::MemoryOrder.seq_cst);")};
    const auto [sym, data, type]{ctx->get_type_sym_info<sema::symbols::node_t>("r", idx)};
    CHECK(type == ctx->get_type(sema::type_kind::VOID_));
}

TEST_CASE("@atomicLoad rejects a non-compile-time order argument") {
    helpers::test_resolver_fail(
        "var ord: builtin::MemoryOrder = builtin::MemoryOrder.seq_cst; var x: i32 = 0; var p: ^mut "
        "i32 = ^mut x; "
        "const r := @atomicLoad(i32, p, ord);",
        sema::diagnostic{"'@atomicLoad' expects 'order' to be a compile-time constant",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 135UZ}});
}

TEST_CASE("@atomicLoad rejects an invalid memory order") {
    helpers::test_resolver_fail(
        fmt::format("{}const r := @atomicLoad(i32, p, builtin::MemoryOrder.release);", PTR_PRELUDE),
        sema::diagnostic{"'@atomicLoad' cannot use memory order 'release'; loads accept relaxed, "
                         "acquire, or seq_cst",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 93UZ}});
}

TEST_CASE("@atomicStore rejects an invalid memory order") {
    helpers::test_resolver_fail(
        fmt::format("{}const r := @atomicStore(p, 1, builtin::MemoryOrder.acquire);", PTR_PRELUDE),
        sema::diagnostic{"'@atomicStore' cannot use memory order 'acquire'; stores accept "
                         "relaxed, release, or seq_cst",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 92UZ}});
}

TEST_CASE("@cmpxchgWeak rejects a release fail_order") {
    const auto src{std::string{PTR_PRELUDE} +
                   "var out: i32 = 0; const r := @cmpxchgWeak(i32, p, "
                   "0, 1, builtin::MemoryOrder.seq_cst, builtin::MemoryOrder.release, "
                   "&mut out);"};
    helpers::test_resolver_fail(
        src,
        sema::diagnostic{"'@cmpxchgWeak' cannot use failure memory order 'release'; 'fail_order' "
                         "may not be release or acq_rel",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 148UZ}});
}

TEST_CASE("@atomicLoad rejects a non-native-width operand type") {
    helpers::test_resolver_fail(
        "var x: u3 = 0; var p: ^mut u3 = ^mut x; const r := @atomicLoad(u3, p, "
        "builtin::MemoryOrder.seq_cst);",
        sema::diagnostic{"'@atomicLoad' operand type 'u3' has no native atomic width on this "
                         "target; 8/16/32/64 bits are always supported, 128 only on "
                         "x86_64/aarch64",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 67UZ}});
}

} // namespace ghoti::tests
