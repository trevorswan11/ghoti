#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/utility.hh>

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

// Two modules spelled alike number their AST nodes alike
constexpr std::string_view TWIN{R"(
    const apply = fn(comptime f: auto): i32 { return f(); };
    pub const get = fn(): i32 { return apply(fn(): i32 { return 1; }); };
)"};

} // namespace

TEST_CASE("A global holding a function as a callable is built with its trampoline", "[uir]") {
    CHECK(helpers::uir_compile_and_run(R"(
        const inc = fn(a: i32): i32 { return a + 1; };
        const dbl = fn(a: i32): i32 { return a * 2; };
        const Fixed: ^fn(a: i32): i32 = ^inc;
        let mut by_ptr: ^fn(a: i32): i32 = ^inc;
        let mut by_val: fn(a: i32): i32 = inc;
        pub const main = fn(): i32 {
            let a = Fixed(1) + (*Fixed)(2);
            let b = by_ptr(3) + by_val(4);
            by_ptr = ^dbl;
            by_val = dbl;
            return a + b + by_ptr(5) + by_val(6);
        };
    )") == (2 + 3) + (4 + 5) + 10 + 12);
}

TEST_CASE("A string spelling a function's symbol is still a string", "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        const symbol = fn(comptime func: auto, comptime name: [:0]u8): void {
            @export(func, .{ .name = name, .linkage = .weak, .visibility = .hidden });
        };
        const g = fn(x: u32): u32 { return x; };
        comptime { symbol(g, "g"); }
    )"));
}

TEST_CASE("Compile-time copies of functions from two modules keep apart", "[uir]") {
    std::string twin_b{TWIN};
    twin_b.replace(twin_b.find("return 1;"), 9, "return 2;");
    CHECK(helpers::uir_compile_and_run(
              R"(
                  import "a.gh" as a;
                  import "b.gh" as b;
                  pub const main = fn(): i32 { return a.get() * 10 + b.get(); };
              )",
              {helpers::mock_file{.path = "a.gh", .source = TWIN},
               helpers::mock_file{.path = "b.gh", .source = twin_b}}) == 12);
}

TEST_CASE("A dereferenced pointer's fields and elements are addressed in its pointee", "[uir]") {
    CHECK(helpers::uir_compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        const TABLE: [3]i32 = .{ 1, 2, 3 };
        const bump_y = fn(pt: ^mut Point): void { (*pt).y = (*pt).y + 10; };
        pub const main = fn(): i32 {
            let mut p = Point{ .x = 1, .y = 2 };
            bump_y(^mut p);
            let t = ^TABLE;
            return p.y + (*t)[2];
        };
    )") == 15);
}

TEST_CASE("A UIR-built program links builtins UIR Sema built", "[uir]") {
    CHECK(helpers::uir_compile_and_run(R"(
        const wide = fn(a: u128, b: u128): u128 { return a / b; };
        pub const main = fn(): i32 {
            let x: u128 = 1;
            return @intCast(i32, wide(x << 100, @as(u128, 3) << 98));
        };
    )") == 1);
}

} // namespace ghoti::tests
