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
                             std::pair{5UZ, 23UZ}});
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
                             std::pair{5UZ, 31UZ}});
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
                             std::pair{3UZ, 28UZ}});
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
                             std::pair{3UZ, 23UZ}});
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
                             std::pair{3UZ, 35UZ}});
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

    SECTION("Indirect function pointer call succeeds") {
        helpers::type_check_and_verify(R"(
            const target := fn(x: i32): i32 {
                return x + 1;
            };
            const f := fn(): i32 {
                var fptr: ^fn(i32): i32 = target;
                return fptr(42);
            };
        )");
    }

    SECTION("Indirect function pointer call with wrong arity fails") {
        helpers::test_checker_fail(
            R"(
            const target := fn(x: i32): i32 {
                return x + 1;
            };
            const f := fn(): i32 {
                var fptr: ^fn(i32): i32 = target;
                return fptr();
            };
        )",
            sema::diagnostic{"Expected 1 arguments, found 0",
                             sema::error::ARITY_MISMATCH,
                             std::pair{6UZ, 23UZ}});
    }

    SECTION("Passing array to slice parameter succeeds") {
        helpers::type_check_and_verify(R"(
            const sum := fn(s: []i32): i32 {
                return s[0];
            };
            const f := fn(): i32 {
                const arr: [3]i32 = [3]i32{1, 2, 3};
                return sum(arr);
            };
        )");
    }

    SECTION("Slice and pointer builtins type check successfully") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^i32): void {
                const p2: ^i32 = @ptrFromInt(^i32, 0x1000UZ);
                const arr: [3]i32 = [3]i32{1, 2, 3};
                const p3: ^i32 = @ptrFromArray(arr);
                const s: []i32 = @sliceFromPtr(p, 10UZ);
                @panic("error");
            };
        )");
    }

    SECTION("Extern union field selection type checks successfully") {
        helpers::type_check_and_verify(R"(
            const RawData := extern union {
                val: i32,
                raw: f64,
            };
            const f := fn(u: RawData): i32 {
                return u.val;
            };
        )");
    }

    SECTION("Packed extern struct type checks successfully") {
        helpers::type_check_and_verify(R"(
            const CPacked := extern packed struct {
                tag: u8,
                data: @alignas(4) i32,
            };
            const f := fn(p: CPacked): i32 {
                return p.data;
            };
        )");
    }
}

TEST_CASE("@setEvalRecursionLimit inside function scope succeeds") {
    helpers::type_check_and_verify(R"(
        pub const test_fn := fn(): void {
            @setEvalRecursionLimit(100);
        };
    )");
}

} // namespace ghoti::tests
