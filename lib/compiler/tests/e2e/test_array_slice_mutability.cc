#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Indexing an array literal rvalue") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return [3]i32{7, 8, 9}[0];
        };
    )") == 7);
}

TEST_CASE("Dynamic index into an array literal rvalue") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var i: usize = 1;
            return [3]i32{7, 8, 9}[i];
        };
    )") == 8);
}

TEST_CASE("Array literal passed as a by-value array argument") {
    CHECK(helpers::compile_and_run(R"(
        const first := fn(a: [3]i32): i32 {
            return a[0];
        };
        pub const main := fn(): i32 {
            return first([3]i32{7, 8, 9});
        };
    )") == 7);
}

TEST_CASE("For loop by value over a local array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            var sum: i32 = 0;
            for (arr) |v| {
                sum = sum + v;
            }
            return sum;
        };
    )") == 6);
}

TEST_CASE("Local array variable read back correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{7, 8, 9};
            var x: i32 = arr[0];
            return x;
        };
    )") == 7);
}

TEST_CASE("Array decayed to an explicit slice-typed local") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            var sl: []i32 = arr;
            return sl[1];
        };
    )") == 2);
}

TEST_CASE("Array built from runtime (non-constant) values") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(a: i32, b: i32, c: i32): i32 {
            var arr := [3]i32{a, b, c};
            return arr[0];
        };
        pub const main := fn(): i32 {
            return make(7, 8, 9);
        };
    )") == 7);
}

TEST_CASE("Struct literal control case") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const make := fn(a: i32, b: i32): i32 {
            var p := Point{ .x = a, .y = b };
            return p.x;
        };
        pub const main := fn(): i32 {
            return make(7, 8);
        };
    )") == 7);
}

TEST_CASE("Struct field with a sized array type") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { items: [3]i32 };
        pub const main := fn(): i32 {
            var b := Box{ .items = [3]i32{7, 8, 9} };
            return b.items[0];
        };
    )") == 7);
}

TEST_CASE("Tagged union scalar field initializer") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .val = 7 };
            return 0;
        };
    )") == 0);
}

TEST_CASE("Tagged union field read") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .val = 42 };
            return u.val;
        };
    )") == 42);
}

TEST_CASE("Tagged union bool field") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .flag = true };
            return if (u.flag) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("Tagged union field with an array type") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { items: [3]i32, flag: bool };
        pub const main := fn(): i32 {
            var u := U{ .items = [3]i32{7, 8, 9} };
            return u.items[0];
        };
    )") == 7);
}

TEST_CASE("Mutable slice parameter mutates the caller's array (var)") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )") == 11);
}

TEST_CASE("Mutable slice parameter mutates the caller's array (const)") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            const slice := [3]mut i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )") == 11);
}

TEST_CASE("mut array decays to a mut slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const first := fn(arr: []mut i32): i32 {
            return arr[0];
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{1, 2, 3};
            return first(slice);
        };
    )") == 1);
}

TEST_CASE("mut array implicitly coerces to a const slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const read_only := fn(arr: []i32): i32 {
            return arr[0];
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{5, 6, 7};
            return read_only(slice);
        };
    )") == 5);
}

TEST_CASE("constCast on a const array preserves identity for mutation") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 100;
        };
        pub const main := fn(): i32 {
            var slice := [3]i32{1, 2, 3};
            bump(@constCast(slice));
            return slice[0];
        };
    )") == 101);
}

TEST_CASE("Directly writing to a mut array's elements") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]mut i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )") == 5);
}

TEST_CASE("mut volatile scalar reads and writes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var v: mut volatile i32 = 42;
            v = v + 1;
            return v;
        };
    )") == 43);
}

TEST_CASE("Passing a const array where a mut slice is required is rejected") {
    helpers::expect_compile_error(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            var slice := [3]i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )");
}

TEST_CASE("Writing to a const array's element is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const arr := [3]i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )");
}

TEST_CASE("Writing to a var array's element without mut is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )");
}

TEST_CASE("Rebinding a const scalar is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x := 5;
            x = 6;
            return x;
        };
    )");
}

} // namespace ghoti::tests
