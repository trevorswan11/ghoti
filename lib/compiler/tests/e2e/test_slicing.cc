#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SUM_HELPER{R"(
const sum := fn(s: []i32): i32 {
    var acc: i32 = 0;
    for (s) |v| { acc = acc + v; }
    return acc;
};
)"};

} // namespace

TEST_CASE("`arr[lo..hi]` produces a subslice of an array") {
    CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
        pub const main := fn(): i32 {
            var arr := [5uz]mut i32{10, 20, 30, 40, 2};
            var lo: usize = 1uz;
            var hi: usize = 4uz;
            return sum(arr[lo..hi]);
        };
    )") == 90);
}

TEST_CASE("`slice[lo..=hi]` is inclusive and re-slices a slice") {
    CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
        pub const main := fn(): i32 {
            var arr := [4uz]mut i32{5, 10, 20, 12};
            var sl: []i32 = arr;
            return sum(sl[1uz..=3uz]) + @as(i32, sl[0uz..2uz].len);
        };
    )") == 42 + 2);
}

TEST_CASE("open-ended range subscripts fill the missing endpoint from the operand") {
    SECTION("over a fixed array") {
        CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
            pub const main := fn(): i32 {
                var arr := [5uz]mut i32{1, 2, 4, 8, 16};
                var lo: usize = 2uz;
                var hi: usize = 4uz;
                return sum(arr[lo..])            // 4 + 8 + 16 = 28
                     + sum(arr[..hi])            // 1 + 2 + 4 + 8 = 15
                     + @as(i32, arr[..].len);    // 5
            };
        )") == 28 + 15 + 5);
    }

    SECTION("over a slice, with an inclusive upper bound") {
        CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
            pub const main := fn(): i32 {
                var arr := [4uz]mut i32{5, 10, 20, 7};
                var sl: []i32 = arr;
                return sum(sl[1uz..])           // 10 + 20 + 7 = 37
                     + sum(sl[..=1uz]);         // 5 + 10 = 15
            };
        )") == 37 + 15);
    }

    SECTION("as a for-loop iterable") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var arr := [4uz]mut i32{3, 4, 5, 6};
                var acc: i32 = 0;
                for (arr[1uz..]) |v| { acc = acc + v; }
                return acc;
            };
        )") == 15);
    }
}

TEST_CASE("an inclusive range with no upper bound is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var arr := [3uz]mut i32{1, 2, 3};
            var lo: usize = 0uz;
            return @as(i32, arr[lo..=].len);
        };
    )");
}

TEST_CASE("an open-ended range outside a subscript is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const r := 2uz..;
            return 0;
        };
    )");
}

TEST_CASE("`@sliceFromPtr(ptr, len)` builds a usable slice") {
    CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
        pub const main := fn(): i32 {
            var arr := [3uz]mut i32{10, 20, 12};
            const s := @sliceFromPtr(^mut arr[0uz], 3uz);
            return sum(s);
        };
    )") == 42);
}

TEST_CASE("a growable buffer over a backing array can append and hand out its items") {
    CHECK(helpers::compile_and_run(std::string{SUM_HELPER} + R"(
        const List := struct {
            buf: [8uz]mut i32,
            len: usize,

            const init := fn(): @this() {
                var l: @this() = undefined;
                l.len = 0uz;
                return l;
            };
            const push := fn(^mut self, v: i32): void {
                self.buf[self.len] = v;
                self.len = self.len + 1uz;
            };
            const items := fn(^self): []i32 {
                return self.buf[0uz..self.len];
            };
        };

        pub const main := fn(): i32 {
            var l := List.init();
            l.push(10);
            l.push(20);
            l.push(12);
            return sum(l.items());
        };
    )") == 42);
}

} // namespace ghoti::tests
