#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Function call type checking") {
    SECTION("Valid call with exact argument types succeeds") {
        helpers::type_check_and_verify(R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10, 20);
            };
        )");
    }

    SECTION("Valid call with implicitly widened argument succeeds") {
        helpers::type_check_and_verify(R"(
            const take_u32 := fn(x: u32): u32 {
                return x;
            };
            const f := fn(b: u8): u32 {
                return take_u32(b);
            };
        )");
    }

    SECTION("Call with wrong argument count fails with ARITY_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10);
            };
        )",
            sema::diagnostic{"Expected 2 arguments, found 1",
                             sema::error::ARITY_MISMATCH,
                             std::pair{5UZ, 24UZ}});
    }

    SECTION("Call with incompatible argument type fails with TYPE_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10, true);
            };
        )",
            sema::diagnostic{"Argument 2 of type 'bool' is not assignable to parameter type 'i32' "
                             "in call to 'add'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{5UZ, 32UZ}});
    }

    SECTION("Valid call to extern function succeeds") {
        helpers::type_check_and_verify(R"(
            extern const puts: fn(^u8): i32;
            const f := fn(msg: ^u8): i32 {
                return puts(msg);
            };
        )");
    }

    SECTION("Call to extern function with wrong argument type fails") {
        helpers::test_checker_fail(
            R"(
            extern const puts: fn(^u8): i32;
            const f := fn(): i32 {
                return puts(42);
            };
        )",
            sema::diagnostic{"Argument 1 of type 'i32' is not assignable to parameter type "
                             "'pointer' in call to 'puts'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 29UZ}});
    }

    SECTION("Valid variadic function call with additional arguments succeeds") {
        helpers::type_check_and_verify(R"(
            const printf := fn(fmt: ^u8, ...): i32 {
                return 0;
            };
            const f := fn(msg: ^u8): i32 {
                return printf(msg, 1, 2, true);
            };
        )");
    }

    SECTION("Valid extern variadic function call succeeds") {
        helpers::type_check_and_verify(R"(
            extern const printf: fn(^u8, ...): i32;
            const f := fn(msg: ^u8): i32 {
                return printf(msg, 42, 3.14);
            };
        )");
    }

    SECTION("Variadic function call with too few arguments fails with ARITY_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            extern const printf: fn(^u8, ...): i32;
            const f := fn(): i32 {
                return printf();
            };
        )",
            sema::diagnostic{"Expected at least 1 arguments, found 0",
                             sema::error::ARITY_MISMATCH,
                             std::pair{3UZ, 24UZ}});
    }

    SECTION("Variadic function call with incompatible fixed argument fails") {
        helpers::test_checker_fail(
            R"(
            extern const printf: fn(^u8, ...): i32;
            const f := fn(): i32 {
                return printf(123, 456);
            };
        )",
            sema::diagnostic{"Argument 1 of type 'i32' is not assignable to parameter type "
                             "'pointer' in call to 'printf'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 36UZ}});
    }

    SECTION("Valid C va builtins type check successfully") {
        helpers::type_check_and_verify(R"(
            const f := fn(ap: ^mut opaque, dest: ^mut opaque): void {
                @cVaStart(ap);
                const val: i32 = @cVaArg(ap, i32);
                @cVaCopy(dest, ap);
                @cVaEnd(ap);
            };
        )");
    }
}

} // namespace ghoti::tests
