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

TEST_CASE("A `for` capture by reference aliases the element it reaches", "[uir]") {
    const auto body{GENERATE(std::string_view{"for (arr) |&mut e| { e *= 2; }"},
                             std::string_view{"for (arr) |^mut e| { *e += 1; }"},
                             std::string_view{"for (s, 0..) |&mut e, i| { if (i > 0) { e = 7; } }"},
                             std::string_view{"for (arr) |&e| { total += e; }"})};
    INFO(body);
    const auto diff{diff_source(fmt::format(R"(
        pub const f = fn(s: []mut i32): i32 {{
            let mut arr = [3]mut i32{{1, 2, 3}};
            let mut total: i32 = 0;
            {}
            return total + arr[0] + arr[2];
        }};
    )",
                                            body))};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A capture by reference writes compile-time memory", "[uir]") {
    const auto body{GENERATE(
        std::string_view{"let a = [_]mut i32{1, 2, 3}; for (a) |&mut e| { e *= 2; } "
                         "return a[0] + a[2];"},
        std::string_view{"let mut a = [_]mut i32{1, 2, 3}; for (a) |^mut e| { *e += 1; } "
                         "return a[2];"},
        std::string_view{"let mut u: U = .{ .a = 4 }; match (u) { .a => |&mut x| { x = 9; }, "
                         ".b => {} } return match (u) { .a => |x| x, .b => 0 };"},
        std::string_view{"let mut p: P = .{ .x = 1, .y = 2 }; match (p) { .{ .y = |&mut y| } "
                         "=> { y = 5; } } return p.y;"},
        std::string_view{"const v: i32 = 3; return match (v) { 3 => |&x| x + 1, _ => 0 };"})};
    INFO(body);
    const auto diff{diff_source(fmt::format(R"(
        const U = union(enum) {{ a: i32, b: void }};
        const P = struct {{ x: i32, y: i32 }};
        const t = fn(): i32 {{ {} }};
        const r = t();
        pub const f = fn(): i32 {{ return r; }};
    )",
                                            body))};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A `for` capture by reference needs storage it may alias", "[uir]") {
    SECTION("a temporary") {
        const auto iterable{GENERATE(std::string_view{"0..3"}, std::string_view{"[_]i32{1, 2}"})};
        INFO(iterable);
        const auto run{helpers::uir_rejected(
            fmt::format("pub const f = fn(): void {{ for ({}) |&e| {{ _ = e; }} }};", iterable))};
        CHECK(only(run, sema::error::ILLEGAL_RVALUE_CAPTURE));
    }

    SECTION("an immutable array") {
        const auto run{helpers::uir_rejected("pub const f = fn(): void { let arr = [_]i32{1, 2}; "
                                             "for (arr) |^mut e| { _ = e; } };")};
        CHECK(only(run, sema::error::ASSIGNMENT_TO_CONST));
    }

    SECTION("a `mut?` capture of a plain slice") {
        const auto run{helpers::uir_rejected(
            "pub const f = fn(s: []i32): void { for (s) |&mut? e| { _ = e; } };")};
        CHECK(only(run, sema::error::ILLEGAL_POLY_MUTABILITY));
    }

    SECTION("a match capture of an immutable element") {
        const auto run{
            helpers::uir_rejected("pub const f = fn(): void { let mut arr = [3]i32{1, 2, 3}; "
                                  "match (arr[1]) { 2 => |&mut v| { v = 3; }, _ => {} } };")};
        CHECK(only(run, sema::error::ASSIGNMENT_TO_CONST));
    }
}

} // namespace ghoti::tests
