#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a C-variadic ghoti function reads every variadic slot in order") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(n: i32, ...): i32 {
            var storage: [4]mut usize = undefined;
            const ap := @ptrCast(^mut opaque, ^mut storage);
            @cVaStart(ap);
            var total: i32 = 0;
            var i: i32 = 0;
            while (i < n) : (i += 1) { total += @cVaArg(ap, i32); }
            @cVaEnd(ap);
            return total;
        };

        pub const main := fn(): i32 {
            return sum(5, 1, 2, 4, 8, 16);
        };
    )") == 31);
}

TEST_CASE("variadic arguments undergo C's default argument promotions") {
    CHECK(helpers::compile_and_run(R"(
        const sum_ints := fn(n: i32, ...): i32 {
            var storage: [4]mut usize = undefined;
            const ap := @ptrCast(^mut opaque, ^mut storage);
            @cVaStart(ap);
            var total: i32 = 0;
            var i: i32 = 0;
            while (i < n) : (i += 1) { total += @cVaArg(ap, i32); }
            @cVaEnd(ap);
            return total;
        };

        const sum_floats := fn(n: i32, ...): i32 {
            var storage: [4]mut usize = undefined;
            const ap := @ptrCast(^mut opaque, ^mut storage);
            @cVaStart(ap);
            var total: f64 = 0.0;
            var i: i32 = 0;
            while (i < n) : (i += 1) { total += @cVaArg(ap, f64); }
            @cVaEnd(ap);
            return @intFromFloat(i32, total);
        };

        pub const main := fn(): i32 {
            const ints := sum_ints(3, @as(u8, 200), @as(i8, -1), 3);
            const floats := sum_floats(2, @as(f32, 1.5), @as(f64, 2.5));
            return ints + floats;
        };
    )") == 206);
}

TEST_CASE("C va builtins reject misuse instead of miscompiling") {
    CHECK(helpers::raised(R"(
        const f := fn(n: i32, ...): void { @cVaStart(n); };
    )",
                          sema::error::TYPE_MISMATCH));

    helpers::expect_compile_error(R"(
        const f := fn(n: i32): i32 {
            var storage: [4]mut usize = undefined;
            @cVaStart(@ptrCast(^mut opaque, ^mut storage));
            return n;
        };
        pub const main := fn(): i32 { return f(1); };
    )");
}

} // namespace ghoti::tests
