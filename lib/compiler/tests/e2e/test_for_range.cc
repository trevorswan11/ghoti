#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a closed range for-loop sums its half-open interval") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for (0..5) |i| { acc = acc + i; }
            return acc;
        };
    )") == 0 + 1 + 2 + 3 + 4);
}

TEST_CASE("a range for-loop endpoint may be a negative literal, and the capture is signed") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for (-2..3) |i| { acc = acc + i; }
            return acc;
        };
    )") == -2 + -1 + 0 + 1 + 2);
}

TEST_CASE("`..` binds looser than arithmetic in a range endpoint") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var n: i32 = 2;
            var acc: i32 = 0;
            for (n - 1..n * 3) |i| { acc = acc + i; }
            return acc;
        };
    )") == 1 + 2 + 3 + 4 + 5);
}

TEST_CASE("`arr[0..arr.len - 1]` groups the subtraction inside the range") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(s: []i32): i32 {
            var a: i32 = 0;
            for (s) |v| { a = a + v; }
            return a;
        };
        pub const main := fn(): i32 {
            var arr := [5uz]mut i32{1, 2, 3, 4, 100};
            return sum(arr[0uz..arr.len - 1uz]);
        };
    )") == 1 + 2 + 3 + 4);
}

TEST_CASE("`for (arr, 0..) |v, i|` enumerates: the sibling array bounds the open range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [4uz]mut i32{10, 20, 30, 40};
            var acc: i32 = 0;
            for (arr, 0..) |v, i| { acc = acc + v + @as(i32, i); }
            return acc;
        };
    )") == (10 + 20 + 30 + 40) + (0 + 1 + 2 + 3));
}

TEST_CASE("an open-ended range for-loop with nothing to bound it is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for (0..) |i| { acc = acc + i; if (i > 3) { break; } }
            return acc;
        };
    )");
}

} // namespace ghoti::tests
