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

TEST_CASE("Bit-packed fields read, write and initialize as the old pipeline's bits", "[uir]") {
    const auto body{GENERATE(
        std::string_view{"let mut p: Pair = .{ .a = n, .b = 5 }; p.b = 9; p.a += 1; "
                         "return @as(i32, p.a) + @as(i32, p.b);"},
        std::string_view{"let mut o: Outer = .{ .lo = 1, .in = .{ .x = 1, .y = 2 }, .f = true }; "
                         "o.in.y = 1; return @as(i32, o.in.x) + @as(i32, o.lo);"},
        std::string_view{"let mut p: Pair = .{ .a = n, .b = 1 }; let q: ^Pair = ^p; "
                         "return @as(i32, q.b);"},
        std::string_view{"let w: Word = .{ .whole = 0x1234 }; return @as(i32, w.low);"},
        std::string_view{
            "const p: Pair = .{ .a = 5, .b = 5 }; return @as(i32, @bitCast(u8, p));"})};
    INFO(body);
    const auto diff{diff_source(fmt::format(R"(
        const Pair = packed struct {{ a: u3, b: u5 }};
        const Inner = packed struct {{ x: u2, y: u2 }};
        const Outer = packed struct {{ lo: u4, in: Inner, f: bool }};
        const Word = packed union {{ whole: u16, low: u8 }};
        pub const f = fn(n: u3): i32 {{ {} }};
    )",
                                            body))};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A bit-packed field written through a pointer keeps its neighbours", "[uir]") {
    // The old pipeline rejects the store, so only UIR Sema's GIR is checked
    const auto run{helpers::uir_accepted(R"(
        const Pair = packed struct { a: u3, b: u5 };
        pub const f = fn(p: ^mut Pair): void { p.b = 3; };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("shl u8"));
    CHECK(gir.contains("and u8"));
    CHECK(gir.contains("or u8"));
}

TEST_CASE("A packed aggregate rejects what it can't lay out as bits", "[uir]") {
    SECTION("an ineligible field") {
        const auto run{
            helpers::uir_rejected("const S = struct { x: i32 }; const P = packed struct { s: S }; "
                                  "pub const f = fn(p: P): void { _ = p; };")};
        CHECK(only(run, sema::error::ILLEGAL_PACKED_FIELD));
    }

    SECTION("an aligned field") {
        const auto run{helpers::uir_rejected("const P = packed struct { @[align(4)] x: u8 }; "
                                             "pub const f = fn(p: P): void { _ = p; };")};
        CHECK(only(run, sema::error::ILLEGAL_PACKED_FIELD));
    }

    SECTION("a backing integer past 128 bits") {
        const auto run{helpers::uir_rejected("const P = packed struct { a: u100, b: u100 }; "
                                             "pub const f = fn(p: P): void { _ = p; };")};
        CHECK(only(run, sema::error::ILLEGAL_PACKED_FIELD));
    }

    SECTION("a field's address") {
        const auto op{GENERATE(std::string_view{"^p.a"}, std::string_view{"&p.a"})};
        INFO(op);
        const auto run{helpers::uir_rejected(
            fmt::format("const P = packed struct {{ a: u8 }}; pub const f = fn(): void {{ "
                        "let mut p: P = .{{ .a = 1 }}; _ = {}; }};",
                        op))};
        CHECK(only(run, sema::error::ILLEGAL_PACKED_FIELD_ADDRESS));
    }
}

TEST_CASE("`@cfg`-gated fields and C types resolve as the old pipeline's", "[uir]") {
    const auto program{GENERATE(std::string_view{R"(
            const S = struct {
                a: i32,
                @cfg (true) { b: i64, c: u8 } else { b: i8 }
                d: bool,
            };
            pub const f = fn(s: S): i64 { return s.b + @as(i64, s.c); };
        )"},
                                std::string_view{R"(
            const E = enum { a, @cfg (false) { b } else { c, d } };
            pub const f = fn(): i32 { return @backingInt(E.d); };
        )"},
                                std::string_view{R"(
            const U = union(enum) { @cfg (true) { x: u32 } y: f32 };
            pub const f = fn(u: U): u32 { return u.x; };
        )"},
                                std::string_view{R"(
            pub const f = fn(a: c_int, b: c_long): c_longlong {
                return @as(c_longlong, a) + @as(c_longlong, b);
            };
        )"})};
    INFO(program);
    const auto diff{diff_source(program)};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("Volatile memory is read and written as the old pipeline's", "[uir]") {
    const auto diff{diff_source(R"(
        const R = struct { a: i32, b: i32 };
        let mut g: volatile i32 = 10;
        let mut regs: volatile R = R{ .a = 1, .b = 2 };
        pub const f = fn(i: usize): i32 {
            let mut x: volatile i32 = 20;
            x = x + g;
            g += 2;
            let p: ^mut volatile i32 = ^mut x;
            *p = 99;
            regs.b = 4;
            let mut copy = g;
            copy += 1;
            return *p + regs.a + copy + @intCast(i32, i);
        };
    )")};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
