#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    auto diff{helpers::diff_pipelines(loader, "main.gh", {}, {})};
    helpers::report_uir_diff(diff);
    return diff;
}

// The same GIR, or the same effects with values computed in another place
[[nodiscard]] auto agrees(const helpers::uir_diff& diff) -> bool {
    return diff.status == helpers::uir_diff_status::MATCH ||
           diff.status == helpers::uir_diff_status::EVAL_ORDER ||
           diff.status == helpers::uir_diff_status::FOLD_PLACEMENT;
}

[[nodiscard]] auto only(const helpers::uir_run& run, sema::error code) -> bool {
    const auto codes{run.codes()};
    return !codes.empty() && std::ranges::all_of(codes, [&](sema::error c) { return c == code; });
}

} // namespace

TEST_CASE("Nested patterns and guards lower as the old pipeline's", "[uir]") {
    const auto body{GENERATE(
        std::string_view{"return match (p) { .{ .x = |x|, .y = |y| } if x == y => 1, "
                         ".{ .x = 0 } => 2, .{ .x = |x| } if x > 5 => 3, _ => 4 };"},
        std::string_view{"return match (u) { .a(|v|), .b(|v|) if v > 0 => v, _ => -1 };"},
        std::string_view{"return match (r) { .ok(.some(|v|)) => v, .ok(.none) => 1, _ => 0 };"},
        std::string_view{"return match (a) { .{ 1, |m|, _ } => m, _ => 0 };"},
        std::string_view{"return match (s) { \"ab\" => 1, .{ .len = 0 } => 2, _ => 3 };"},
        std::string_view{"return match (n) { lo..=hi => 1, 0 => 2, _ => 3 };"},
        std::string_view{"return match (p.x) { 0.. |v| => v, _ => 0 };"})};
    INFO(body);
    const auto diff{diff_source(fmt::format(R"(
        const P = struct {{ pub x: i32, pub y: i32 }};
        const U = union(enum) {{ a: i32, b: i32 }};
        const O = union(enum) {{ some: i32, none: void }};
        const R = union(enum) {{ ok: O, err: u8 }};
        pub const f = fn(p: P, u: U, r: R, a: [3]i32, s: []u8, n: i32, lo: i32, hi: i32): i32 {{
            {}
        }};
    )",
                                            body))};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A destructuring checks its pattern and binds its names", "[uir]") {
    SECTION("an irrefutable one") {
        const auto diff{diff_source(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const f = fn(p: P): i32 { let .{ .x = |a|, .y = |b| } = p; return a + b; };
        )")};
        INFO(diff.detail);
        CHECK(agrees(diff));
    }

    SECTION("a refutable one needs an `else`") {
        const auto run{helpers::uir_rejected(R"(
            const U = union(enum) { a: i32, b: void };
            pub const f = fn(u: U): i32 { let .a(|v|) = u; return v; };
        )")};
        CHECK(only(run, sema::error::ILLEGAL_MATCH_PATTERN));
    }

    SECTION("an `else` that carries on") {
        const auto run{helpers::uir_rejected(R"(
            const U = union(enum) { a: i32, b: void };
            pub const f = fn(u: U): i32 { let .a(|v|) = u else {}; return v; };
        )")};
        CHECK(only(run, sema::error::ILLEGAL_MATCH_PATTERN));
    }
}

TEST_CASE("A match on a type picks its arm in a runtime body", "[uir]") {
    SECTION("the arm it picks") {
        const auto diff{diff_source(R"(
            const T = i64;
            pub const f = fn(): i32 { return match (T) { i32 => 1, i64 => 2, _ => 9 }; };
        )")};
        INFO(diff.detail);
        CHECK(agrees(diff));
    }

    SECTION("the rules it follows") {
        const auto body{GENERATE(std::string_view{"i32 => 1, bool => 2"},
                                 std::string_view{"5 => 1, _ => 0"},
                                 std::string_view{"i32 => |x| 1, _ => 0"})};
        INFO(body);
        const auto run{helpers::uir_rejected(fmt::format(
            "const T = i32; pub const f = fn(): i32 {{ return match (T) {{ {} }}; }};", body))};
        CHECK(only(run, sema::error::ILLEGAL_MATCH_PATTERN));
    }
}

TEST_CASE("A union tagged by an enum converts to that enum", "[uir]") {
    const auto diff{diff_source(R"(
        const K = enum : u8 { a = 3, b = 7, c = 9 };
        const U = union(K) { c: bool, a: i32, b: u8 };
        pub const f = fn(u: U): K { let k: K = u; return k; };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));

    const auto run{helpers::uir_rejected("const K = enum { a, b }; const U = union(K) { a: i32 }; "
                                         "pub const f = fn(u: U): void { _ = u; };")};
    CHECK(only(run, sema::error::ILLEGAL_UNION_TAG));
}

} // namespace ghoti::tests
