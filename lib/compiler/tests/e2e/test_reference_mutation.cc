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

TEST_CASE("Reading a &i32 reference parameter without an explicit deref") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 41;
            const readit := fn(c: &i32): i32 {
                return c + 1;
            };
            return readit(&x);
        };
    )") == 42);
}

TEST_CASE("Writing through a &mut i32 reference parameter without an explicit deref") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: &mut i32): void {
                c = 99;
            };
            setit(&mut x);
            return x;
        };
    )") == 99);
}

TEST_CASE("Reading and writing a &mut i32 reference without an explicit deref, combined") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const addone := fn(c: &mut i32): void {
                c = c + 1;
            };
            addone(&mut x);
            return x;
        };
    )") == 6);
}

TEST_CASE("Compound assignment through a &mut i32 reference without an explicit deref") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const addone := fn(c: &mut i32): void {
                c += 1;
            };
            addone(&mut x);
            return x;
        };
    )") == 6);
}

TEST_CASE("Reading a struct field through a &Point reference without an explicit deref") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 41 };
            const get_y := fn(pt: &Point): i32 {
                return pt.y;
            };
            return get_y(&p);
        };
    )") == 41);
}

TEST_CASE("A local reference variable reads with value semantics") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const r: &mut i32 = &mut x;
            r = r + 10;
            return x;
        };
    )") == 15);
}

TEST_CASE("Passing a plain value where a &mut i32 parameter is expected is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: &mut i32): void {
                c = 99;
            };
            setit(x);
            return x;
        };
    )");
}

TEST_CASE("Declaring a reference-typed variable from a plain value is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var r: &i32 = x;
            return r;
        };
    )");
}

TEST_CASE("Taking & of an already-reference-typed value is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const setit := fn(c: &mut i32): void {
                c = 99;
            };
            const r: &mut i32 = &mut x;
            setit(&mut r);
            return x;
        };
    )");
}

TEST_CASE("Passing an existing reference directly aliases the same referent") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const addone := fn(c: &mut i32): void {
                c = c + 1;
            };
            const r: &mut i32 = &mut x;
            addone(r);
            return x;
        };
    )") == 6);
}

} // namespace ghoti::tests
