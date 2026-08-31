#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A non-null pointer is truthy in an if condition") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            return if (p) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("A null pointer is falsy in an if condition") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return if (p) 1 else 0;
        };
    )") == 0);
}

TEST_CASE("!ptr is true for a null pointer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return if (!p) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("!ptr is false for a non-null pointer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            return if (!p) 1 else 0;
        };
    )") == 0);
}

TEST_CASE("A pointer condition drives a while loop to completion") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            var p: ^mut i32 = ^mut x;
            var count: i32 = 0;
            while (p) {
                count = count + 1;
                p = nullptr;
            };
            return count;
        };
    )") == 1);
}

TEST_CASE("A pointer condition drives a do-while loop") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            var count: i32 = 0;
            do {
                count = count + 1;
            } while (p);
            return count;
        };
    )") == 1);
}

TEST_CASE("An opaque pointer is truthy in an if condition") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            const p: ^opaque = @ptrCast(^opaque, ^x);
            return if (p) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("A pointer is truthy as an 'and' / 'or' operand") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            var n: ^i32 = nullptr;
            const with_and := if (p and n == nullptr) 1 else 0;
            const with_or := if (n or p) 1 else 0;
            return with_and * 10 + with_or;
        };
    )") == 11);
}

TEST_CASE("@as(bool, ptr) yields true for a non-null pointer and false for null") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            const live := @as(bool, p);
            p = nullptr;
            const dead := @as(bool, p);
            return if (live and !dead) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("@as(bool, int) tests the integer against zero") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a := @as(bool, 7);
            const b := @as(bool, 0);
            return if (a and !b) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("Implicitly assigning a pointer to a bool binding is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            const b: bool = p;
            return if (b) 1 else 0;
        };
    )");

    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            var b: bool = p;
            return if (b) 1 else 0;
        };
    )");
}

TEST_CASE("Passing a pointer where a bool parameter is expected is rejected") {
    helpers::expect_compile_error(R"(
        const takes_bool := fn(b: bool): i32 {
            return if (b) 1 else 0;
        };
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            return takes_bool(p);
        };
    )");
}

TEST_CASE("A null-terminated linked list is walked while the cursor pointer is truthy") {
    CHECK(helpers::compile_and_run(R"(
        const Node := struct { val: i32, next: ^Node };
        pub const main := fn(): i32 {
            var n3 := Node{ .val = 3, .next = nullptr };
            var n2 := Node{ .val = 2, .next = ^n3 };
            var n1 := Node{ .val = 1, .next = ^n2 };
            var cur: ^Node = ^n1;
            var sum: i32 = 0;
            while (cur) {
                sum = sum + cur.val;
                cur = cur.next;
            };
            return sum;
        };
    )") == 6);
}

TEST_CASE("if (ptr) guards a defer-bearing scope") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            var p: ^mut i32 = ^mut x;
            if (p) {
                defer *p = *p + 1;
                *p = 41;
            };
            return x;
        };
    )") == 42);
}

} // namespace ghoti::tests
