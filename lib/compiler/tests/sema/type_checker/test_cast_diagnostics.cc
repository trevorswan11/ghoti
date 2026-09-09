#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Cast rejection diagnostics in type checker") {
    SECTION("Narrowing integer conversion explains truncation and suggests @intCast or @truncate") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: i64): void {
                var y: i32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'i64' to 'i32' (narrowing conversion from "
                "'i64' to 'i32' may truncate high bits; use @intCast for a checked conversion or "
                "@truncate to discard high bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 29UZ}});
    }

    SECTION("Signedness change with same width explains value change and suggests @intCast or "
            "@bitCast") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: u32): void {
                var y: i32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'u32' to 'i32' (conversion from 'u32' to "
                "'i32' changes signedness and may change the represented value; use @intCast for a "
                "checked conversion or @bitCast to reinterpret the bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 29UZ}});
    }

    SECTION("Narrowing and sign change explains both and suggests @intCast or @truncate") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: u64): void {
                var y: i32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'u64' to 'i32' (conversion from 'u64' to "
                "'i32' narrows width and changes signedness; use @intCast for a checked conversion "
                "or @truncate to discard high bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 29UZ}});
    }

    SECTION("Signed to unsigned widening explains negative values and suggests @intCast") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: i16): void {
                var y: u32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'i16' to 'u32' (conversion from signed "
                "'i16' "
                "to unsigned 'u32' cannot represent negative values; use @intCast for a checked "
                "conversion)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 29UZ}});
    }

    SECTION("Float narrowing suggests @as") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: f64): void {
                var y: f32 = x;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'f64' to 'f32' (narrowing conversion from "
                "'f64' to 'f32' may lose precision; use @as for an explicit conversion)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 29UZ}});
    }

    SECTION("Call argument mismatch includes cast reason") {
        helpers::test_checker_fail(
            R"(
            const take_i32 := fn(x: i32): void {};
            const f := fn(x: i64): void {
                take_i32(x);
            };
        )",
            sema::diagnostic{
                "Argument 1 of type 'i64' is not assignable to parameter type 'i32' in call to "
                "'take_i32' (narrowing conversion from 'i64' to 'i32' may truncate high bits; use "
                "@intCast for a checked conversion or @truncate to discard high bits)",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 25UZ}});
    }

    SECTION("Return value mismatch includes cast reason") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: i64): i32 {
                return x;
            };
        )",
            sema::diagnostic{
                "Return value of type 'i64' is not assignable to function return type 'i32' "
                "(narrowing conversion from 'i64' to 'i32' may truncate high bits; use @intCast "
                "for a checked conversion or @truncate to discard high bits)",
                sema::error::RETURN_TYPE_MISMATCH,
                std::pair{2UZ, 16UZ}});
    }
}

} // namespace ghoti::tests
