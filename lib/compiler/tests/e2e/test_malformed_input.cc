#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("unary plus is an identity on numeric operands") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 2;
            return +a + +3;
        };
    )") == 5);

    CHECK(helpers::raised("const x := +true;", sema::error::OPERATOR_TYPE_MISMATCH));
}

TEST_CASE("a closed range is not a first-class value") {
    CHECK(helpers::raised(R"(
        const f := fn(): void { const r := 1..5; _ = r; };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
}

TEST_CASE("an unsuffixed integer literal typed `constexpr_float` folds as a float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const b: @TypeOf(0.0) = 1;
            if (b != 1.5f64) { return 7; }
            return 0;
        };
    )") == 7);
}

TEST_CASE("a type argument for an `auto` value parameter is rejected, not miscompiled") {
    CHECK(helpers::raised(R"(
        const f := fn(v: auto): u8 { return 1; };
        const g := fn(): u8 { return f(u16); };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`@bitCast` rejects operands of different sizes") {
    CHECK(helpers::raised("const x := @bitCast(i32, @as(i64, 1));", sema::error::TYPE_MISMATCH));
}

TEST_CASE("pointer arithmetic needs a sized pointee") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var p: ^opaque = undefined;
            const q := p + 1;
            _ = q;
            return 0;
        };
    )");
}

TEST_CASE("range bounds must be integers") {
    CHECK(helpers::raised(R"(
        const f := fn(): void { for (0.5..2.5) |i| { _ = i; } };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("constant shifts by a negative or oversized amount are rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { const x: i32 = 1; return x << -1; };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { return 1 << 100000; };
    )");
}

TEST_CASE("enum member values must fit the underlying type and be distinct") {
    CHECK(helpers::raised("const E := enum : u8 { a = 300 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum : u8 { a = -1 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum : u8 { a = 254, b, c };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum { a = 1, b = 1 };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("an unvalued enum member continues from its predecessor") {
    CHECK(helpers::compile_and_run(R"(
        const E := enum { a, b = 5, c };
        pub const main := fn(): i32 {
            const e: E = .c;
            return @intCast(i32, @backingInt(e)) + match (e) { .a => 0, .b => 0, .c => 1, };
        };
    )") == 7);
}

TEST_CASE("assignments need an assignable left side") {
    CHECK(helpers::raised("const f := fn(): void { 5 = 6; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("a value cannot be used as a type") {
    CHECK(helpers::raised("const x := 5; const f := fn(): void { var y: x = 1; _ = y; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`impl X for T` requires X to be an interface") {
    CHECK(helpers::raised("const S := struct {}; impl i32 for S {};", sema::error::TYPE_MISMATCH));
}

TEST_CASE("array lengths must be non-negative and addressable") {
    CHECK(helpers::raised("const f := fn(): void { var a: [-1]u8 = undefined; _ = a; };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(
        "const f := fn(): void { var a: [18446744073709551615]u8 = undefined; _ = a; };",
        sema::error::TYPE_MISMATCH));
}

TEST_CASE("runaway generic type construction is reported, not a stack overflow") {
    helpers::expect_compile_error(R"(
        const f := fn(T: type): type { return f([]T); };
        const X := f(i32);
        pub const main := fn(): i32 { var x: X = undefined; _ = x; return 0; };
    )");
}

TEST_CASE("a local that is read before its declaration is rejected") {
    CHECK(helpers::raised(R"(
        const f := fn(): i32 { var b: i32 = a + 1; var a: i32 = 7; return b; };
    )",
                          sema::error::ILLEGAL_FIELD_ORDER_DEPENDENCY));
}

TEST_CASE("a local function can call an earlier local function") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const g := fn(): i32 { return 4; };
            const f := fn(): i32 { return g() + 1; };
            return f();
        };
    )") == 5);
}

TEST_CASE("a very long constant string literal compiles") {
    std::string src{"pub const main := fn(): i32 { const s := \""};
    src.append(100'000, 'a');
    src += "\"; return @intCast(i32, s.len % 256); };";
    CHECK(helpers::compile_and_run(src) == 100'000 % 256);
}

TEST_CASE("pointer arithmetic steps by whole pointees in either operand order") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: [3]i32 = .{ 1, 2, 3 };
            const p: ^i32 = @ptrFromArray(a);
            var n: usize = 2;
            const q := n + p;
            return *(p + 2) + *(1 + p) + *(q - 1);
        };
    )") == 7);
}

TEST_CASE("type operands only take part in type identity comparisons") {
    CHECK(helpers::raised("const f := fn(): bool { return u8 < 5; };",
                          sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::compile_and_run(R"(
        const pick := fn(T: type): i32 { if (T == u8) { return 1; } return 2; };
        pub const main := fn(): i32 { return pick(u8) + pick(i32); };
    )") == 3);
}

TEST_CASE("a builtin must be called") {
    CHECK(helpers::raised("const f := fn(): void { const g := @sizeOf; _ = g; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("casts need a type target and a value operand") {
    CHECK(helpers::raised("const f := fn(): i32 { return @intCast(i32, ); };",
                          sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised("const f := fn(x: u8): i32 { return @intCast(5, x); };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::compile_and_run(R"(
        const widen := fn(T: type, x: u8): T { return @as(T, x); };
        pub const main := fn(): i32 { return widen(i32, 4); };
    )") == 4);
}

TEST_CASE("a function declared to return a value cannot return nothing") {
    CHECK(helpers::raised("const f := fn(): usize { return; };", sema::error::RETURN_TYPE_MISMATCH));
    CHECK(helpers::raised("const f := fn(): constexpr_int { const x := 1; _ = x; };",
                          sema::error::RETURN_TYPE_MISMATCH));
}

TEST_CASE("an anonymous aggregate cannot be used mid-expression") {
    CHECK(helpers::raised("const C := struct { id: i32 }.len;", sema::error::TYPE_MISMATCH));
}

TEST_CASE("a generic function cannot be exported") {
    CHECK(helpers::raised("export const f := fn(x: auto): i32 { return 0; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a constant index past a known length is rejected, but a sentinel slot is readable") {
    CHECK(helpers::raised("const f := fn(): i32 { const a: [2]i32 = .{ 1, 2 }; return a[5]; };",
                          sema::error::SLICE_OUT_OF_BOUNDS));
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "abc";
            return @intCast(i32, s[3]) + 9;
        };
    )") == 9);
}

TEST_CASE("errors inside a parameterized impl's methods are reported") {
    helpers::expect_compile_error(R"(
        const Box := fn(T: type): type { return struct { val: T }; };
        impl(T: type) Box(T) {
            pub const get := fn(&self): Nope { return self.val; };
        }
        pub const main := fn(): i32 {
            const b: Box(i32) = .{ .val = 3 };
            return b.get();
        };
    )");
}

TEST_CASE("a method can call itself through its owning type") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            const fact := fn(n: i32): i32 { if (n <= 1) { return 1; } return n * S.fact(n - 1); };
        };
        pub const main := fn(): i32 { return S.fact(5); };
    )") == 120);
}

TEST_CASE("an unused void-typed global has no storage to emit") {
    CHECK(helpers::compile_and_run(R"(
        const choose := fn(x: auto): auto { return; };
        const r := choose(99);
        pub const main := fn(): i32 { return 0; };
    )") == 0);
}

TEST_CASE("a variable of type noreturn is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { var v: noreturn = undefined; _ = v; return 0; };
    )");
}

TEST_CASE("large aggregates pass and return by value through direct and indirect calls") {
    CHECK(helpers::compile_and_run(R"(
        const Big := struct { data: [100000]mut u8 };
        const make := fn(v: u8): Big { var b: Big = undefined; b.data[7] = v; return b; };
        const read := fn(b: Big): u8 { return b.data[7]; };
        const via := fn(f: fn(b: Big): u8, b: Big): u8 { return f(b); };
        pub const main := fn(): i32 {
            const b := make(9);
            var arr: [100000]mut u8 = undefined;
            arr[3] = 4;
            const copy := arr;
            return @as(i32, read(b)) + @as(i32, via(read, b)) + @as(i32, copy[3]);
        };
    )") == 22);
}

TEST_CASE("a type passed to a generic parameter bound to a value type is rejected") {
    CHECK(helpers::raised(R"(
        const dup := fn(T: type, val: T): T { return val + val; };
        pub const main := fn(): i32 { const b := dup(u8, u8); return 0; };
    )", sema::error::TYPE_USED_AS_VALUE));
}

TEST_CASE("errors inside a parameterized impl method are reported once per site") {
    constexpr auto prefix{R"(
        const P := fn(A: type): type { return struct { a: A }; };
        const I := interface { pub const l := fn(&self): i32; };
        impl(A: type) I for P(A) { pub const l := fn(&self): i32 { )"};
    constexpr auto suffix{R"( }; }
        pub const main := fn(): i32 {
            var p: P(i32) = .{ .a = 1 };
            var q: P(u8) = .{ .a = 2 };
            return p.l() + q.l();
        };
    )"};
    const auto with_body{[&](std::string_view body) {
        return std::string{prefix} + std::string{body} + suffix;
    }};
    const auto range_diags{helpers::resolve_diags(with_body("return 0..1;"))};
    CHECK(std::ranges::count(range_diags.codes, sema::error::ILLEGAL_OPEN_RANGE) == 1);
    const auto undeclared{helpers::resolve_diags(with_body("return nope;"))};
    CHECK(std::ranges::count(undeclared.codes, sema::error::UNDECLARED_IDENTIFIER) == 1);
    CHECK(helpers::raised(with_body("return @intCast(i32, u8);"), sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised(with_body("const r := self.a[..]; return 0;"),
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a parameterized impl method body types against its concrete instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const P := fn(A: type): type { return struct { a: A }; };
        const I := interface { pub const l := fn(&self): i32; };
        impl(A: type) I for P(A) {
            pub const l := fn(&self): i32 { return @intCast(i32, self.a[1]) + @intCast(i32, self.a.len); };
        }
        pub const main := fn(): i32 { var p: P([]u8) = .{ .a = "hey" }; return p.l(); };
    )") == 104);
}

TEST_CASE("a module-scope initializer must match its annotation") {
    CHECK(helpers::raised("const g: i32 = true;", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const f: i32 = fn(): i32 { return 0; };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const S := struct { const X: bool = 3; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("an inline aggregate type in value position has no runtime value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            _ = struct {};
            enum { a };
            const T := blk: { break :blk union { a: i32 }; };
            return 7;
        };
    )") == 7);
    helpers::expect_compile_error(R"(
        const f := fn(): void { _ = if (true) struct {} else struct {}; };
    )");
}

TEST_CASE("`@tagName` requires an enum or tagged union value") {
    constexpr auto decls{R"(
        const S := struct { x: i32 };
        const E := enum { a, b };
        const R := extern union { a: i32 };
    )"};
    const auto with_operand{[&](std::string_view operand) {
        return std::string{decls} + "const f := fn(e: E, pe: ^E): void { _ = @tagName(" +
               std::string{operand} + "); };";
    }};
    CHECK(helpers::raised(with_operand("u8"), sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised(with_operand("5"), sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(with_operand("S{ .x = 1 }"), sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(with_operand("pe"), sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(with_operand("R{ .a = 1 }"), sema::error::TYPE_MISMATCH));
}

TEST_CASE("a loop body error inside a generic reports cleanly on every instantiation") {
    CHECK(helpers::raised(R"(
        constexpr eql := fn(T: type, a: []T, b: []T): bool {
            for (a, b) |x, y| { if (x != missing) { return false; } }
            return true;
        };
        constexpr starts := fn(T: type, h: []T, n: []T): bool { return eql(T, h[0..n.len], n); };
        const D := struct { count_x: f32, count_y: u32 };
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for constexpr (@typeInfo(D).@"struct".fields) |field| {
                if constexpr (starts(u8, field.name, "count_")) { acc += 1; }
            }
            return acc;
        };
    )", sema::error::UNDECLARED_IDENTIFIER));
}

} // namespace ghoti::tests
