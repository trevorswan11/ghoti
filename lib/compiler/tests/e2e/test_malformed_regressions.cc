#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

// Inputs the mutation fuzzer turned into compiler crashes; each must now be a clean diagnostic
namespace ghoti::tests {

TEST_CASE("An attribute argument with a builtin arity error is reported, not a crash") {
    helpers::expect_compile_error(R"(
        const misaligned := fn(T: type): i32 {
            @[align(if (@sizeOf() > 4) 64 else 32)]
            var buf: [4]u8 = undefined;
            _ = buf;
            return 0;
        };
        pub const main := fn(): i32 { return misaligned(i32); };
    )");
}

TEST_CASE("@hasField rejects a non-type owner and a non-string name") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 { return @intFromBool(@hasField(Point, Point)); };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { return @intFromBool(@hasField("x", "z")); };
    )");
}

TEST_CASE("An untyped float can't be returned from an integer function") {
    helpers::expect_compile_error("pub const main := fn(): i32 { return 1.5; };");
}

TEST_CASE("Unary operators reject a type operand") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x := -i64;
            _ = x;
            return 0;
        };
    )");
}

TEST_CASE("A pack parameter named as a return type is an error, not a crash") {
    helpers::expect_compile_error(R"(
        const g := fn(rest...): rest {};
        const f := fn(void...): void { g(rest...); };
        const use := fn(): void { f(1, 2, 3); };
    )");
}

TEST_CASE("Cast builtins reject an undefined operand") {
    helpers::expect_compile_error(R"(
        const f := fn(x: i32): u32 {
            var a: u32 = @bitCast(x);
            a = @bitCast(undefined);
            return a;
        };
    )");
}

TEST_CASE("Code after a match whose arms all return is dead, not miscompiled") {
    CHECK(helpers::compile_and_run(R"(
        const pick := fn(n: i32): i32 {
            const v := match (n) {
                _ => { return 99; },
            };
            return v + 1;
        };
        pub const main := fn(): i32 { return pick(5); };
    )") == 99);
}

TEST_CASE("A field can't have type noreturn") {
    helpers::expect_compile_error(R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits == 7) { linux_only: i32 }
            else                { fallback: noreturn }
            z: i32,
        };
        var s: S = undefined;
    )");
    helpers::expect_compile_error("var x: [2]noreturn = undefined;");
}

TEST_CASE("A constexpr loop over a condition-less if constexpr is checked without crashing") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            loop constexpr {
                if constexpr fn(): i32 (n == 5) { break; }
                n = n + 1;
            }
            return n;
        };
    )");
}

TEST_CASE("Match arms yielding a type beside arms yielding values are an error") {
    helpers::expect_compile_error(R"(
        const P := struct { a: i32 };
        const f := fn(n: i32): P {
            return match (n) {
                0 => P,
                _ => P{ .a = 1 },
            };
        };
    )");
}

} // namespace ghoti::tests
