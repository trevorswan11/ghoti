#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Dereferencing a null pointer with *p traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return *p;
        };
    )") != 0);
}

TEST_CASE("Writing through a null pointer with *p = v traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^mut i32 = nullptr;
            *p = 7;
            return 0;
        };
    )") != 0);
}

TEST_CASE("Reading a field through a null pointer traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p: ^Point = nullptr;
            return p.x;
        };
    )") != 0);
}

TEST_CASE("Writing a field through a null pointer traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p: ^mut Point = nullptr;
            p.x = 3;
            return 0;
        };
    )") != 0);
}

TEST_CASE("Indexing through a null pointer traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return p[0];
        };
    )") != 0);
}

TEST_CASE("A null pointer hidden behind a function parameter still traps on deref", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        const deref := fn(q: ^i32): i32 {
            return *q;
        };
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return deref(p);
        };
    )") != 0);
}

TEST_CASE("A null intermediate in a chained field access traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        const Node := struct { val: i32, next: ^Node };
        pub const main := fn(): i32 {
            var head := Node{ .val = 1, .next = nullptr };
            var cur: ^Node = ^head;
            return cur.next.val;
        };
    )") != 0);
}

TEST_CASE("Valid pointer dereferences are unaffected by the null-pointer safety check") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var pt := Point{ .x = 40, .y = 2 };
            var p: ^mut Point = ^mut pt;
            p.x = p.x + 1;
            var arr: [2uz]mut i32 = [2uz]mut i32{10, 20};
            var ap: ^mut i32 = @ptrFromArray(arr);
            ap[1] = ap[1] + 1;
            return p.x + p.y + arr[1];   // 41 + 2 + 21
        };
    )") == 64);
}

} // namespace ghoti::tests
