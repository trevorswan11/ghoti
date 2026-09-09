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

} // namespace ghoti::tests
