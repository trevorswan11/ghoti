#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Dereference-assignment through a &mut i32 parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: &mut i32): void {
                *c = 99;
            };
            setit(&mut x);
            return x;
        };
    )") == 99);
}

TEST_CASE("Dereference-assignment combined with arithmetic through a &mut i32 parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const addone := fn(c: &mut i32): void {
                *c = *c + 1;
            };
            addone(&mut x);
            return x;
        };
    )") == 6);
}

TEST_CASE("Compound dereference-assignment through a &mut i32 parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const addone := fn(c: &mut i32): void {
                *c += 1;
            };
            addone(&mut x);
            return x;
        };
    )") == 6);
}

TEST_CASE("Dereference-assignment through a raw ^mut i32 parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: ^mut i32): void {
                *c = 99;
            };
            setit(^mut x);
            return x;
        };
    )") == 99);
}

TEST_CASE("Writing through a &i32 (non-mut) reference is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: &i32): void {
                *c = 99;
            };
            setit(&x);
            return x;
        };
    )");
}

TEST_CASE("Indexing through a &[N]i32 reference parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr: [3uz]i32 = [_]i32{10, 20, 30};
            const sum_first_two := fn(a: &[3uz]i32): i32 {
                return a[0] + a[1];
            };
            return sum_first_two(&arr);
        };
    )") == 30);
}

TEST_CASE("Mutating an element through a &mut [N]mut i32 reference parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr: [3uz]mut i32 = [3uz]mut i32{10, 20, 30};
            const bump_first := fn(a: &mut [3uz]mut i32): void {
                a[0] = a[0] + 5;
            };
            bump_first(&mut arr);
            return arr[0];
        };
    )") == 15);
}

TEST_CASE("Reading an element through a &[N]mut i32 reference to a mut array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr: [3uz]mut i32 = [3uz]mut i32{10, 20, 30};
            const sum_first_two := fn(a: &[3uz]mut i32): i32 {
                return a[0] + a[1];
            };
            return sum_first_two(&arr);
        };
    )") == 30);
}

} // namespace ghoti::tests
