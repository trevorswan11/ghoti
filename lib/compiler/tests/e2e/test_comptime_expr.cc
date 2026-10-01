#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "compiler/syntax/error.hh"

#include "helpers/ast.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/formatter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`comptime <expr>` folds its operand at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(v: i32): i32 { return v * 2; };
        const Pair = struct { a: i32, b: i32 };
        const make = fn(n: i32): Pair { return .{ .a = n, .b = twice(n) }; };
        pub const main = fn(): i32 {
            let p = comptime make(3);
            let arr = comptime [3]i32{ 1, 2, twice(3) };
            return comptime twice(10) + p.b + arr[2];
        };
    )") == 20 + 6 + 6);
}

TEST_CASE("`comptime` binds like a prefix operator") {
    // Only `twice(21)` is compile-time; `+ x` stays a runtime addition
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(v: i32): i32 { return v * 2; };
        pub const main = fn(): i32 {
            let mut x: i32 = 0;
            x += 1;
            let a = comptime twice(21) + x;
            let b = comptime (1 + 2) * 3;
            return a + b;
        };
    )") == 43 + 9);
    // Parenthesizing pulls the runtime value into the compile-time operand
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 1;
            return comptime (x + 1);
        };
    )");
}

TEST_CASE("a `void` operand runs its compile-time checks and emits nothing") {
    CHECK(helpers::compile_and_run(R"(
        const check = fn(): void { @assert(1 + 1 == 2); };
        pub const main = fn(): i32 {
            comptime check();
            return 0;
        };
    )") == 0);
    helpers::expect_compile_error(R"(
        const check = fn(): void { @assert(1 + 1 == 3); };
        pub const main = fn(): i32 {
            comptime check();
            return 0;
        };
    )");
}

TEST_CASE("`comptime` is redundant where evaluation already happens at compile time") {
    const auto redundant = [](usize col) -> syntax::diagnostic {
        return {"Redundant 'comptime': this expression is already evaluated at compile time",
                syntax::error::REDUNDANT_COMPTIME,
                std::pair{0UZ, col}};
    };
    helpers::test_parser_fail("const x = comptime 1;", redundant(10));
    helpers::test_parser_fail("let x = comptime 1;", redundant(8));
    helpers::test_parser_fail("comptime f();", redundant(0));
    helpers::test_parser_fail("const f = fn(): void { const y = comptime 1; };", redundant(33));
    helpers::test_parser_fail("const f = fn(): void { comptime { let y = comptime 1; } };",
                              redundant(42));
    helpers::test_parser_fail("const f = fn(): void { _ = comptime comptime 1; };", redundant(36));
    helpers::test_parser_fail("const f = fn(): void { if comptime (comptime true) {} };",
                              redundant(36));
}

TEST_CASE("`comptime` needs an operand") {
    helpers::test_parser_fail("const f = fn(): void { comptime; };",
                              syntax::diagnostic{"Expected an expression after 'comptime'",
                                                 syntax::error::PREFIX_MISSING_OPERAND,
                                                 std::pair{0UZ, 23UZ}});
}

TEST_CASE("`comptime` folds per instantiation of a generic") {
    CHECK(helpers::compile_and_run(R"(
        const width = fn(T: type): usize { return @sizeOf(T) * 8; };
        const bits = fn(x: auto): usize {
            _ = x;
            return comptime width(@TypeOf(x));
        };
        const Box = fn(T: type): type {
            return struct {
                v: T,
                pub const size = fn(&self): usize { return comptime width(T); };
            };
        };
        pub const main = fn(): i32 {
            let a: u8 = 1;
            let b: i64 = 2;
            let box: Box(u16) = .{ .v = 3 };
            return @intCast(i32, bits(a) + bits(b) + box.size());
        };
    )") == 8 + 64 + 16);
}

TEST_CASE("`comptime` calls a function from another module") {
    CHECK(helpers::compile_and_run(R"(
        import "util.gh" as util;
        pub const main = fn(): i32 { return comptime util.cube(3); };
    )",
                                   {{"util.gh", R"(
        pub const cube = fn(v: i32): i32 { return v * v * v; };
    )"}}) == 27);
}

TEST_CASE("a function holding `comptime <expr>` still folds when called at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(v: i32): i32 { return v * 2; };
        const outer = fn(v: i32): i32 { return v + comptime twice(5); };
        const folded = outer(1);
        pub const main = fn(): i32 { return folded; };
    )") == 11);
}

TEST_CASE("a condition-less `if comptime` in the operand takes the compile-time arm") {
    CHECK(helpers::compile_and_run(R"(
        const where = fn(): i32 { return if comptime 1 else 2; };
        pub const main = fn(): i32 {
            let folded = comptime where();
            let runtime = where();
            return folded * 10 + runtime;
        };
    )") == 12);
}

TEST_CASE("`comptime <expr>` feeds a `comptime` parameter and an array size") {
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(v: usize): usize { return v * 2; };
        const fill = fn(comptime n: usize): usize { return n; };
        pub const main = fn(): i32 {
            let arr: [comptime twice(2)]u8 = .{ 1, 2, 3, 4 };
            return @intCast(i32, fill(comptime twice(3)) + arr.len);
        };
    )") == 6 + 4);
}

TEST_CASE("a `const` function's param read only inside `comptime <expr>` is inferred") {
    CHECK(helpers::compile_and_run(R"(
        const scaled = fn(n: i32): i32 { return comptime n * 4; };
        pub const main = fn(): i32 { return scaled(5); };
    )") == 20);
}

TEST_CASE("`comptime <expr>` round-trips through the formatter") {
    helpers::round_trips("const f = fn(): i32 {\n    let a = comptime g(1) + x;\n"
                         "    comptime check();\n    return comptime (a + 1);\n};\n");
}

} // namespace ghoti::tests
