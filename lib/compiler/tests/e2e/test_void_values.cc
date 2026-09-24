#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a void parameter can initialize a union payload") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: void, b: i32 };
        const make := fn(v: void): U { return .{ .a = v }; };
        pub const main := fn(): i32 {
            return if (make({}) == .a) 7 else 1;
        };
    )") == 7);
}

TEST_CASE("an all-void union can be constructed through a void parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Both := union { yes: void, no: void };
        const make := fn(v: void): Both { return .{ .yes = v }; };
        pub const main := fn(): i32 {
            var b: Both = .{ .no = {} };
            if (b != .no) return 1;
            b = make({});
            return if (b == .yes) 9 else 2;
        };
    )") == 9);
}

TEST_CASE("a generic union constructor taking T compiles when T is void") {
    CHECK(helpers::compile_and_run(R"(
        const R := fn(T: type, E: type): type {
            return union {
                ok: T,
                err: E,
                pub constexpr of := fn(v: T): @This() { return .{ .ok = v }; };
            };
        };
        const run := fn(fail: bool): R(void, i32) {
            if (fail) return .{ .err = 3 };
            return .of({});
        };
        pub const main := fn(): i32 {
            const a: R(i32, bool) = .of(5);
            const ok_val := match (a) { .ok => |v| v, .err => 0 };
            const failed := match (run(true)) { .ok => 0, .err => |e| e };
            return if (run(false) == .ok) ok_val * 10 + failed else 1;
        };
    )") == 53);
}

} // namespace ghoti::tests
