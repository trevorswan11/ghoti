#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("constexpr-fits coercion diagnostics in type checker (D3)") {
    SECTION("constexpr integer out of range in const decl reports LITERAL_OUT_OF_RANGE") {
        helpers::test_checker_fail(
            R"(
            constexpr OVER: usize = 400;
            const f := fn(): void {
                const bad: u8 = OVER;
            };
        )",
            sema::diagnostic{"integer value 400 is out of range for type 'u8'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 32UZ}});
    }

    SECTION("constexpr integer out of range in var decl reports LITERAL_OUT_OF_RANGE") {
        helpers::test_checker_fail(
            R"(
            constexpr OVER: usize = 400;
            const f := fn(): void {
                var bad: u8 = OVER;
            };
        )",
            sema::diagnostic{"integer value 400 is out of range for type 'u8'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 30UZ}});
    }

    SECTION("constexpr negative integer into unsigned reports LITERAL_OUT_OF_RANGE") {
        helpers::test_checker_fail(
            R"(
            constexpr NEG: i32 = -5;
            const f := fn(): void {
                const bad: u32 = NEG;
            };
        )",
            sema::diagnostic{"integer value -5 is out of range for type 'u32'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 33UZ}});
    }

    SECTION("constexpr integer out of range in call argument reports LITERAL_OUT_OF_RANGE") {
        helpers::test_checker_fail(
            R"(
            constexpr OVER: usize = 400;
            const take_u8 := fn(x: u8): void {};
            const f := fn(): void {
                take_u8(OVER);
            };
        )",
            sema::diagnostic{"integer value 400 is out of range for type 'u8'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{4UZ, 24UZ}});
    }

    SECTION("Runtime variable narrowing fails with TYPE_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: usize = 200;
                var n: u8 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'usize' to 'u8' (narrowing conversion from "
                "'usize' to 'u8' may truncate high bits; use @intCast for a checked conversion or "
                "@truncate to discard high bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 28UZ}});
    }

    SECTION("Runtime variable sign mismatch fails with TYPE_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: i32 = 42;
                var n: u32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'i32' to 'u32' (conversion from 'i32' to "
                "'u32' changes signedness and may change the represented value; use @intCast for a "
                "checked conversion or @bitCast to reinterpret the bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 29UZ}});
    }
}

TEST_CASE("compile-time floats that round to infinity in their type are rejected") {
    SECTION("a literal that rounds to infinity in f32") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                const bad: f32 = 1e39;
            };
        )",
            sema::diagnostic{"literal is out of range for type 'f32'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{2UZ, 33UZ}});
    }

    SECTION("an integer literal past f16's range") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                const bad: f16 = 65520;
            };
        )",
            sema::diagnostic{"literal is out of range for type 'f16'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{2UZ, 33UZ}});
    }

    SECTION("a suffixed literal past its own width") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                const bad := 1e39f32;
            };
        )",
            sema::diagnostic{"literal is out of range for type 'f32'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{2UZ, 29UZ}});
    }

    SECTION("a literal argument past a parameter's range") {
        helpers::test_checker_fail(
            R"(
            const g := fn(x: f16): f16 { return x; };
            const f := fn(): void {
                _ = g(70000.0);
            };
        )",
            sema::diagnostic{"literal is out of range for type 'f16'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 22UZ}});
    }

    SECTION("a named constant past a float binding's range") {
        helpers::test_checker_fail(
            R"(
            constexpr BIG := 1e300;
            const f := fn(): void {
                const bad: f32 = BIG;
            };
        )",
            sema::diagnostic{"float value 1e+300 is out of range for type 'f32'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 33UZ}});
    }

    SECTION("a folded expression past a float binding's range") {
        helpers::test_checker_fail(
            R"(
            constexpr BIG := 1e300;
            const f := fn(): void {
                const bad: f16 = BIG * 2.0;
            };
        )",
            sema::diagnostic{"float value 2e+300 is out of range for type 'f16'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 33UZ}});
    }

    SECTION("an @as operand past the target's range") {
        helpers::test_checker_fail(
            R"(
            constexpr BIG := 1e300;
            const f := fn(): void {
                const bad := @as(f32, BIG);
            };
        )",
            sema::diagnostic{"float value 1e+300 is out of range for type 'f32'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 38UZ}});
    }

    SECTION("a folded @floatFromInt past the target's range") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                const bad := @floatFromInt(f16, 100000);
            };
        )",
            sema::diagnostic{"float value 100000 is out of range for type 'f16'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{2UZ, 48UZ}});
    }

    SECTION("an integer constant past a float binding's range") {
        helpers::test_checker_fail(
            R"(
            constexpr BIG := 340282366920938463463374607431768211455;
            const f := fn(): void {
                const bad: f16 = BIG;
            };
        )",
            sema::diagnostic{"float value 3.402823669209385e+38 is out of range for type 'f16'",
                             sema::error::LITERAL_OUT_OF_RANGE,
                             std::pair{3UZ, 33UZ}});
    }
}

} // namespace ghoti::tests
