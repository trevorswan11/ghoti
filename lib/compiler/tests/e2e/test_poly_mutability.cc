#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Runs `t()` both at runtime and folded through `comptime t()`; each must give `expected`
auto check_both(std::string_view defs, u32 expected) -> void {
    const auto runtime{fmt::format("{}\npub const main = fn(): i32 {{ return t(); }};", defs)};
    const auto folded{
        fmt::format("{}\npub const main = fn(): i32 {{ return comptime t(); }};", defs)};
    CHECK(helpers::compile_and_run(runtime) == expected);
    CHECK(helpers::compile_and_run(folded) == expected);
}

auto expect_error(std::string_view source, sema::error code) -> void {
    const auto [ctx, idx]{helpers::expect_compile_error(source)};
    const auto& diags{ctx->root_mod.diagnostics.as<sema::diagnostics>()};
    CHECK(std::ranges::any_of(diags, [&](const auto& diag) { return diag.get_error() == code; }));
}

constexpr std::string_view VEC{R"(
    const Vec = struct {
        items: [3]mut i32,
        pub const at = fn(&mut? self, i: usize): &mut? i32 { return &mut? self.items[i]; };
        pub const first = fn(&mut? self): ^mut? i32 { return ^mut? self.items[0]; };
        // A `mut?` call inside a `mut?` function passes its own mutability along
        pub const last = fn(&mut? self): &mut? i32 { return self.at(2); };
    };
)"};

} // namespace

TEST_CASE("a `mut?` accessor is mutable through a `let mut` receiver") {
    check_both(fmt::format(R"({}
        const t = fn(): i32 {{
            let mut v: Vec = .{{ .items = .{{1, 2, 3}} }};
            let r = v.at(1);
            r = 20;
            let p = v.first();
            *p += 10;
            let l = v.last();
            l = 30;
            return v.items[0] + v.items[1] + v.items[2];
        }};
    )",
                           VEC),
               61);
}

TEST_CASE("a `mut?` accessor reads through a `let` receiver") {
    check_both(fmt::format(R"({}
        const t = fn(): i32 {{
            let v: Vec = .{{ .items = .{{4, 5, 6}} }};
            return v.at(2) + v.last() + *v.first();
        }};
    )",
                           VEC),
               16);
}

TEST_CASE("a `mut?` slice parameter follows its argument") {
    check_both(R"(
        const get = fn(s: []mut? i32, i: usize): &mut? i32 { return &mut? s[i]; };
        const t = fn(): i32 {
            let a = [_]mut i32{7, 8, 9};
            let e = get(a, 2);
            e = 90;
            let b = [_]i32{1, 2};
            return a[2] + get(b, 1);
        };
    )",
               92);
}

TEST_CASE("a `match` capture by `&mut?` reborrows the payload") {
    check_both(R"(
        const U = union { a: i32, b: void };
        const payload = fn(u: &mut? U): &mut? i32 {
            return match (u) { .a => |&mut? v| v, .b => unreachable };
        };
        const t = fn(): i32 {
            let mut u: U = .{ .a = 1 };
            let p = payload(&mut u);
            p = 41;
            return match (u) { .a => |v| v + 1, .b => 0 };
        };
    )",
               42);
}

TEST_CASE("writing through a `mut?` view is an error") {
    expect_error("const f = fn(x: &mut? i32): void { x = 5; };", sema::error::ASSIGNMENT_TO_CONST);
    expect_error("const f = fn(s: []mut? i32): void { s[0] = 1; };",
                 sema::error::ASSIGNMENT_TO_CONST);
}

TEST_CASE("`&mut?` of a place not reached through a `mut?` parameter is an error") {
    expect_error(R"(
        const f = fn(x: &mut? i32): &mut? i32 { let mut y: i32 = 1; return &mut? y; };
    )",
                 sema::error::ILLEGAL_POLY_MUTABILITY);
    expect_error(R"(
        const f = fn(s: []i32): void { for (s) |&mut? e| { _ = e; } };
    )",
                 sema::error::ILLEGAL_POLY_MUTABILITY);
}

TEST_CASE("a `mut?` view only takes its mutability from a `mut?` parameter") {
    // A mutable or constant view can't become a `mut?` one
    expect_error(R"(
        const f = fn(x: &mut? i32): &mut? i32 { let mut y: i32 = 1; return &mut y; };
    )",
                 sema::error::RETURN_TYPE_MISMATCH);
    expect_error("const f = fn(x: &i32): &mut? i32 { return x; };",
                 sema::error::ILLEGAL_POLY_MUTABILITY);
    expect_error("const S = struct { r: &mut? i32 };", sema::error::ILLEGAL_POLY_MUTABILITY);
    expect_error("const U = union { a: []mut? u8, b: void };",
                 sema::error::ILLEGAL_POLY_MUTABILITY);
    expect_error("let g: ^mut? i32 = undefined;", sema::error::ILLEGAL_POLY_MUTABILITY);
}

TEST_CASE("an interface's `mut?` method needs a `mut?` impl and can't be called through `dyn`") {
    constexpr std::string_view iface{R"(
        const I = interface { pub const get = fn(&mut? self): &mut? i32; };
        const S = struct { v: i32 };
    )"};
    expect_error(fmt::format(R"({}
        impl I for S {{ pub const get = fn(&mut self): &mut i32 {{ return &mut self.v; }}; }}
    )",
                             iface),
                 sema::error::IMPL_SIGNATURE_MISMATCH);
    expect_error(fmt::format(R"({}
        impl I for S {{ pub const get = fn(&mut? self): &mut? i32 {{ return &mut? self.v; }}; }}
        const use = fn(x: &dyn I): i32 {{ return x.get(); }};
    )",
                             iface),
                 sema::error::ILLEGAL_POLY_MUTABILITY);
    CHECK(helpers::compile_and_run(fmt::format(R"({}
        impl I for S {{ pub const get = fn(&mut? self): &mut? i32 {{ return &mut? self.v; }}; }}
        pub const main = fn(): i32 {{
            let mut s: S = .{{ .v = 3 }};
            let r = s.get();
            r = 9;
            return s.v;
        }};
    )",
                                               iface)) == 9);
}

TEST_CASE("a generic function's `mut?` result takes the call's mutability") {
    check_both(R"(
        const first = fn(T: type, s: []mut? T): &mut? T { return &mut? s[0]; };
        const t = fn(): i32 { let mut a = [2]mut i32{ 1, 1 }; first(i32, a[0..]) = 2; return a[0]; };
    )",
               2);
}

} // namespace ghoti::tests
