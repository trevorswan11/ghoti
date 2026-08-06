#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Cast type checking") {
    SECTION("Valid pointer cast succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^mut i32): ^i32 {
                return @ptrCast(^i32, p);
            };
        )");
    }

    SECTION("Valid const pointer cast succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^i32): ^mut i32 {
                return @constCast(p);
            };
        )");
    }

    SECTION("Casting away const from pointer without constCast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^i32): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )",
            sema::diagnostic{"Cannot cast away const from pointer without @constCast",
                             sema::error::ILLEGAL_CONST_CAST,
                             std::pair{2UZ, 42UZ}});
    }

    SECTION("Casting between typed pointer and opaque pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const to_opaque := fn(p: ^i32): ^opaque {
                return @ptrCast(^opaque, p);
            };
            const from_opaque := fn(p: ^mut opaque): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )");
    }

    SECTION("Casting away const via opaque pointer without constCast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^opaque): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )",
            sema::diagnostic{"Cannot cast away const from pointer without @constCast",
                             sema::error::ILLEGAL_CONST_CAST,
                             std::pair{2UZ, 42UZ}});
    }
}

TEST_CASE("Implicit numeric widening conversions") {
    helpers::type_check_and_verify(R"(
        pub const test_fn := fn(): void {
            const val_u8: u8 = 'a';
            const val_u32: u32 = val_u8;
            const val_u64: u64 = val_u32;
            const val_usize: usize = val_u8;

            const val_i32: i32 = 42;
            const val_i64: i64 = val_i32;
            const val_isize: isize = val_i32;

            const val_f32: f32 = 3.14f;
            const val_f64: f64 = val_f32;
        };
    )");
}

TEST_CASE("Explicit numeric casting via @as") {
    helpers::type_check_and_verify(R"(
        pub const test_fn := fn(): void {
            const big_u64: u64 = 1000;
            const narrow_u8 := @as(u8, big_u64);
            const narrow_u32 := @as(u32, big_u64);

            const s_i32: i32 = -5;
            const u_u32 := @as(u32, s_i32);
            const back_i32 := @as(i32, u_u32);

            const float_val: f64 = 9.99;
            const int_from_float := @as(i32, float_val);
            const float_from_int := @as(f64, int_from_float);
        };
    )");
}

TEST_CASE("Non-widenable implicit integer narrowing fails without @as") {
    helpers::test_checker_fail(
        R"(
        pub const test_fn := fn(): void {
            var big: u64 = 100;
            var small: u8 = big;
        };
    )",
        sema::diagnostic{"Type mismatch in store: cannot assign 'u64' to 'u8'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{3UZ, 28UZ}});
}

TEST_CASE("Implicit reference type construction from value types") {
    helpers::type_check_and_verify(R"(
        pub const take_ref := fn(r: &i32): i32 {
            return *r;
        };

        pub const take_mut_ref := fn(r: &mut i32): void {
            *r = 42;
        };

        pub const caller := fn(): i32 {
            var x: i32 = 10;
            const res1 := take_ref(x);
            take_mut_ref(x);
            var r_var: &i32 = x;
            return res1 + *r_var;
        };

        const val: i32 = 10;
        const r_var: &i32 = val;
    )");
}

TEST_CASE("Implicit reference construction in struct field initialization") {
    helpers::type_check_and_verify(R"(
        const RefHolder := struct { r: &i32 };
        const val: i32 = 10;
        const h := RefHolder{ .r = val };
    )");
}

TEST_CASE("Const mismatch in implicit mutable reference produces diagnostic error") {
    helpers::test_checker_fail(
        R"(
            pub const take_mut_ref := fn(r: &mut i32): void {
                *r = 42;
            };

            pub const caller := fn(const_ref: &i32): void {
                take_mut_ref(const_ref);
            };
        )",
        sema::diagnostic{"Argument 1 of type 'reference' is not assignable to parameter type "
                         "'reference' in call to 'take_mut_ref'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{6UZ, 29UZ}});
}

} // namespace ghoti::tests
