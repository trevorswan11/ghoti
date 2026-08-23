#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Writing a struct field through a var local") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            p.y = p.y + 10;
            return p.y;
        };
    )") == 12);
}

TEST_CASE("Writing a struct field through a const local is rejected") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            const p := Point{ .x = 1, .y = 2 };
            p.y = p.y + 10;
            return p.y;
        };
    )");
}

TEST_CASE("Writing a struct field through a &mut Point reference parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            const bump_y := fn(pt: &mut Point): void {
                pt.y = pt.y + 10;
            };
            bump_y(&mut p);
            return p.y;
        };
    )") == 12);
}

TEST_CASE("Writing a struct field through a &Point (non-mut) reference is rejected") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            const bump_y := fn(pt: &Point): void {
                pt.y = pt.y + 10;
            };
            bump_y(&p);
            return p.y;
        };
    )");
}

TEST_CASE("Writing a struct field through a ^mut Point pointer parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            const bump_y := fn(pt: ^mut Point): void {
                (*pt).y = (*pt).y + 10;
            };
            bump_y(^mut p);
            return p.y;
        };
    )") == 12);
}

TEST_CASE("Writing a struct field through a ^Point (non-mut) pointer is rejected") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            const bump_y := fn(pt: ^Point): void {
                (*pt).y = (*pt).y + 10;
            };
            bump_y(^p);
            return p.y;
        };
    )");
}

TEST_CASE("Writing a nested struct field through a var local") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { px: i32, py: i32 };
        const Box := struct { corner: Point, size: i32 };
        pub const main := fn(): i32 {
            var b := Box{ .corner = Point{ .px = 1, .py = 2 }, .size = 10 };
            b.corner.py = b.corner.py + 100;
            return b.corner.py;
        };
    )") == 102);
}

TEST_CASE("Writing a struct field inside a mut array element") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var arr: [2uz]mut Point = [2uz]mut Point{
                Point{ .x = 1, .y = 2 }, Point{ .x = 3, .y = 4 }
            };
            arr[0].y = arr[0].y + 100;
            return arr[0].y;
        };
    )") == 102);
}

TEST_CASE("Writing a struct field inside a non-mut array element is rejected") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var arr: [2uz]Point = [2uz]Point{
                Point{ .x = 1, .y = 2 }, Point{ .x = 3, .y = 4 }
            };
            arr[0].y = arr[0].y + 100;
            return arr[0].y;
        };
    )");
}

TEST_CASE("Writing a struct field inside a mut slice element") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var arr: [2uz]mut Point = [2uz]mut Point{
                Point{ .x = 1, .y = 2 }, Point{ .x = 3, .y = 4 }
            };
            var sl: []mut Point = arr;
            const bump := fn(s: []mut Point): void {
                s[0].y = s[0].y + 100;
            };
            bump(sl);
            return sl[0].y;
        };
    )") == 102);
}

TEST_CASE("Writing a union field through a var local") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .val = 5 };
            u.val = u.val + 10;
            return u.val;
        };
    )") == 15);
}

TEST_CASE("Writing a different union field than the active one updates the active variant") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            var u := U{ .a = 1 };
            u.b = 99;
            return match (u) {
                .a => 1,
                .b => 2,
            };
        };
    )") == 2);
}

TEST_CASE("Writing a union field through a const local is rejected") {
    helpers::expect_compile_error(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            const u := U{ .val = 5 };
            u.val = u.val + 10;
            return u.val;
        };
    )");
}

TEST_CASE("Mutating a by-value struct parameter's local copy persists across accesses") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            const modify := fn(pt: Point): i32 {
                pt.y = pt.y + 10;
                return pt.y;
            };
            var p := Point{ .x = 1, .y = 2 };
            return modify(p);
        };
    )") == 12);
}

TEST_CASE("Mutating a by-value struct parameter does not affect the caller's copy") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            const modify := fn(pt: Point): void {
                pt.y = pt.y + 10;
            };
            var p := Point{ .x = 1, .y = 2 };
            modify(p);
            return p.y;
        };
    )") == 2);
}

} // namespace ghoti::tests
