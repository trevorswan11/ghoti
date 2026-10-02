#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("unary plus is an identity on numeric operands") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = 2;
            return +a + +3;
        };
    )") == 5);

    CHECK(helpers::raised("const x = +true;", sema::error::OPERATOR_TYPE_MISMATCH));
}

TEST_CASE("a closed range is not a first-class value") {
    CHECK(helpers::raised(R"(
        const f = fn(): void { let r = 1..5; _ = r; };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
}

TEST_CASE("an unsuffixed integer literal typed `comptime_float` folds as a float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let b: @TypeOf(0.0) = 1;
            if (b != 1.5f64) { return 7; }
            return 0;
        };
    )") == 7);
}

TEST_CASE("a type argument for an `auto` value parameter is rejected, not miscompiled") {
    CHECK(helpers::raised(R"(
        const f = fn(v: auto): u8 { return 1; };
        const g = fn(): u8 { return f(u16); };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`@bitCast` rejects operands of different sizes") {
    CHECK(helpers::raised("const x = @bitCast(i32, @as(i64, 1));", sema::error::TYPE_MISMATCH));
}

TEST_CASE("pointer arithmetic needs a sized pointee") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut p: ^opaque = undefined;
            let q = p + 1;
            _ = q;
            return 0;
        };
    )");
}

TEST_CASE("range bounds must be integers") {
    CHECK(helpers::raised(R"(
        const f = fn(): void { for (0.5..2.5) |i| { _ = i; } };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("constant shifts by a negative or oversized amount are rejected") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { let x: i32 = 1; return x << -1; };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { return 1 << 100000; };
    )");
}

TEST_CASE("enum member values must fit the underlying type and be distinct") {
    CHECK(helpers::raised("const E = enum : u8 { a = 300 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E = enum : u8 { a = -1 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E = enum : u8 { a = 254, b, c };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E = enum { a = 1, b = 1 };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("an unvalued enum member continues from its predecessor") {
    CHECK(helpers::compile_and_run(R"(
        const E = enum { a, b = 5, c };
        pub const main = fn(): i32 {
            let e: E = .c;
            return @intCast(i32, @backingInt(e)) + match (e) { .a => 0, .b => 0, .c => 1, };
        };
    )") == 7);
}

TEST_CASE("assignments need an assignable left side") {
    CHECK(helpers::raised("const f = fn(): void { 5 = 6; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("a value cannot be used as a type") {
    CHECK(helpers::raised("const x = 5; const f = fn(): void { let mut y: x = 1; _ = y; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`impl X for T` requires X to be an interface") {
    CHECK(helpers::raised("const S = struct {}; impl i32 for S {};", sema::error::TYPE_MISMATCH));
}

TEST_CASE("array lengths must be non-negative and addressable") {
    CHECK(helpers::raised("const f = fn(): void { let mut a: [-1]u8 = undefined; _ = a; };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(
        "const f = fn(): void { let mut a: [18446744073709551615]u8 = undefined; _ = a; };",
        sema::error::TYPE_MISMATCH));
}

TEST_CASE("runaway generic type construction is reported, not a stack overflow") {
    helpers::expect_compile_error(R"(
        const f = fn(T: type): type { return f([]T); };
        const X = f(i32);
        pub const main = fn(): i32 { let mut x: X = undefined; _ = x; return 0; };
    )");
}

TEST_CASE("a local that is read before its declaration is rejected") {
    CHECK(helpers::raised(R"(
        const f = fn(): i32 { let mut b: i32 = a + 1; let mut a: i32 = 7; return b; };
    )",
                          sema::error::ILLEGAL_FIELD_ORDER_DEPENDENCY));
}

TEST_CASE("a local function can call an earlier local function") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const g = fn(): i32 { return 4; };
            const f = fn(): i32 { return g() + 1; };
            return f();
        };
    )") == 5);
}

TEST_CASE("a very long constant string literal compiles") {
    std::string src{"pub const main = fn(): i32 { const s = \""};
    src.append(100'000, 'a');
    src += "\"; return @intCast(i32, s.len % 256); };";
    CHECK(helpers::compile_and_run(src) == 100'000 % 256);
}

TEST_CASE("pointer arithmetic steps by whole pointees in either operand order") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: [3]i32 = .{ 1, 2, 3 };
            let p: ^i32 = @ptrFromArray(a);
            let mut n: usize = 2;
            let q = n + p;
            return *(p + 2) + *(1 + p) + *(q - 1);
        };
    )") == 7);
}

TEST_CASE("type operands only take part in type identity comparisons") {
    CHECK(helpers::raised("const f = fn(): bool { return u8 < 5; };",
                          sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::compile_and_run(R"(
        const pick = fn(T: type): i32 { if (T == u8) { return 1; } return 2; };
        pub const main = fn(): i32 { return pick(u8) + pick(i32); };
    )") == 3);
}

TEST_CASE("a builtin must be called") {
    CHECK(helpers::raised("const f = fn(): void { let g = @sizeOf; _ = g; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("casts need a type target and a value operand") {
    CHECK(helpers::raised("const f = fn(): i32 { return @intCast(i32, ); };",
                          sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised("const f = fn(x: u8): i32 { return @intCast(5, x); };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::compile_and_run(R"(
        const widen = fn(T: type, x: u8): T { return @as(T, x); };
        pub const main = fn(): i32 { return widen(i32, 4); };
    )") == 4);
}

TEST_CASE("a function declared to return a value cannot return nothing") {
    CHECK(helpers::raised("const f = fn(): usize { return; };", sema::error::RETURN_TYPE_MISMATCH));
    CHECK(helpers::raised("const f = fn(): comptime_int { let x = 1; _ = x; };",
                          sema::error::RETURN_TYPE_MISMATCH));
}

TEST_CASE("an anonymous aggregate cannot be used mid-expression") {
    CHECK(helpers::raised("const C = struct { id: i32 }.len;", sema::error::TYPE_MISMATCH));
}

TEST_CASE("a generic function cannot be exported") {
    CHECK(helpers::raised("export const f = fn(x: auto): i32 { return 0; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a constant index past a known length is rejected, but a sentinel slot is readable") {
    CHECK(helpers::raised("const f = fn(): i32 { let a: [2]i32 = .{ 1, 2 }; return a[5]; };",
                          sema::error::SLICE_OUT_OF_BOUNDS));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let s = "abc";
            return @intCast(i32, s[3]) + 9;
        };
    )") == 9);
}

TEST_CASE("errors inside a parameterized impl's methods are reported") {
    helpers::expect_compile_error(R"(
        const Box = fn(T: type): type { return struct { val: T }; };
        impl(T: type) Box(T) {
            pub const get = fn(&self): Nope { return self.val; };
        }
        pub const main = fn(): i32 {
            let b: Box(i32) = .{ .val = 3 };
            return b.get();
        };
    )");
}

TEST_CASE("a method can call itself through its owning type") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct {
            const fact = fn(n: i32): i32 { if (n <= 1) { return 1; } return n * S.fact(n - 1); };
        };
        pub const main = fn(): i32 { return S.fact(5); };
    )") == 120);
}

TEST_CASE("an unused void-typed global has no storage to emit") {
    CHECK(helpers::compile_and_run(R"(
        const choose = fn(x: auto): auto { return; };
        const r = choose(99);
        pub const main = fn(): i32 { return 0; };
    )") == 0);
}

TEST_CASE("a variable of type noreturn is rejected") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { let mut v: noreturn = undefined; _ = v; return 0; };
    )");
}

TEST_CASE("large aggregates pass and return by value through direct and indirect calls") {
    CHECK(helpers::compile_and_run(R"(
        const Big = struct { data: [100000]mut u8 };
        const make = fn(v: u8): Big { let mut b: Big = undefined; b.data[7] = v; return b; };
        const read = fn(b: Big): u8 { return b.data[7]; };
        const via = fn(f: fn(b: Big): u8, b: Big): u8 { return f(b); };
        pub const main = fn(): i32 {
            let b = make(9);
            let mut arr: [100000]mut u8 = undefined;
            arr[3] = 4;
            let copy = arr;
            return @as(i32, read(b)) + @as(i32, via(read, b)) + @as(i32, copy[3]);
        };
    )") == 22);
}

TEST_CASE("a type passed to a generic parameter bound to a value type is rejected") {
    CHECK(helpers::raised(R"(
        const dup = fn(T: type, val: T): T { return val + val; };
        pub const main = fn(): i32 { let b = dup(u8, u8); return 0; };
    )",
                          sema::error::TYPE_USED_AS_VALUE));
}

TEST_CASE("errors inside a parameterized impl method are reported once per site") {
    constexpr auto prefix{R"(
        const P = fn(A: type): type { return struct { a: A }; };
        const I = interface { pub const l = fn(&self): i32; };
        impl(A: type) I for P(A) { pub const l = fn(&self): i32 { )"};
    constexpr auto suffix{R"( }; }
        pub const main = fn(): i32 {
            let mut p: P(i32) = .{ .a = 1 };
            let mut q: P(u8) = .{ .a = 2 };
            return p.l() + q.l();
        };
    )"};
    const auto     with_body{
        [&](std::string_view body) { return std::string{prefix} + std::string{body} + suffix; }};
    const auto range_diags{helpers::resolve_diags(with_body("return 0..1;"))};
    CHECK(std::ranges::count(range_diags.codes, sema::error::ILLEGAL_OPEN_RANGE) == 1);
    const auto undeclared{helpers::resolve_diags(with_body("return nope;"))};
    CHECK(std::ranges::count(undeclared.codes, sema::error::UNDECLARED_IDENTIFIER) == 1);
    CHECK(helpers::raised(with_body("return @intCast(i32, u8);"), sema::error::TYPE_USED_AS_VALUE));
    CHECK(
        helpers::raised(with_body("const r = self.a[..]; return 0;"), sema::error::TYPE_MISMATCH));
}

TEST_CASE("a parameterized impl method body types against its concrete instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const P = fn(A: type): type { return struct { a: A }; };
        const I = interface { pub const l = fn(&self): i32; };
        impl(A: type) I for P(A) {
            pub const l = fn(&self): i32 { return @intCast(i32, self.a[1]) + @intCast(i32, self.a.len); };
        }
        pub const main = fn(): i32 { let mut p: P([]u8) = .{ .a = "hey" }; return p.l(); };
    )") == 104);
}

TEST_CASE("a module-scope initializer must match its annotation") {
    CHECK(helpers::raised("const g: i32 = true;", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const f: i32 = fn(): i32 { return 0; };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const S = struct { const X: bool = 3; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("an inline aggregate type in value position has no runtime value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            _ = struct {};
            enum { a };
            let T = blk: { break :blk union { a: i32 }; };
            return 7;
        };
    )") == 7);
    helpers::expect_compile_error(R"(
        const f = fn(): void { _ = if (true) struct {} else struct {}; };
    )");
}

TEST_CASE("`@tagName` requires an enum or tagged union value") {
    constexpr auto decls{R"(
        const S = struct { x: i32 };
        const E = enum { a, b };
        const R = extern union { a: i32 };
    )"};
    const auto     with_operand{[&](std::string_view operand) {
        return std::string{decls} + "const f = fn(e: E, pe: ^E): void { _ = @tagName(" +
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
        const eql = fn(T: type, a: []T, b: []T): bool {
            for (a, b) |x, y| { if (x != missing) { return false; } }
            return true;
        };
        const starts = fn(T: type, h: []T, n: []T): bool { return eql(T, h[0..n.len], n); };
        const D = struct { count_x: f32, count_y: u32 };
        pub const main = fn(): i32 {
            let mut acc: i32 = 0;
            for comptime (@typeInfo(D).@"struct".fields) |field| {
                if comptime (starts(u8, field.name, "count_")) { acc += 1; }
            }
            return acc;
        };
    )",
                          sema::error::UNDECLARED_IDENTIFIER));
}

TEST_CASE("misused names and types report diagnostics instead of crashing") {
    // A bare sibling field inside a method
    CHECK(helpers::raised(R"(
        const S = struct { n: i32, pub const set = fn(&mut self): void { n = 3; }; };
    )",
                          sema::error::UNDECLARED_IDENTIFIER));
    // A member of an uncalled builtin
    CHECK(helpers::raised(
        "pub const main = fn(): i32 { let x: usize = 3; return @intCast.foo(i32, x); };",
        sema::error::TYPE_MISMATCH));
    // A reference to a bare interface
    CHECK(helpers::raised(R"(
        const W = interface { pub const wr = fn(&self): i32; };
        const use = fn(w: &W): i32 { return w.wr(); };
    )",
                          sema::error::INTERFACE_NOT_A_VALUE));
    // Members of `type` itself and of a function
    CHECK(helpers::raised("pub const main = fn(): i32 { return type.a; };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("pub const main = fn(): void { let q = fn(): void {}.p; };",
                          sema::error::TYPE_MISMATCH));
    // A value bound to a `type` annotation or passed to a `T: type` parameter
    CHECK(helpers::raised("const p: type = 5;", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(R"(
        const Box = fn(T: type): type { return struct { val: T }; };
        pub const main = fn(): i32 { let mut q: i32 = 1; let b: Box(q) = .{ .val = 9 }; return 0; };
    )",
                          sema::error::TYPE_MISMATCH));
    // A builtin in type position
    CHECK(helpers::raised("const make = fn(b: i32): @compileError { return 1; };",
                          sema::error::TYPE_MISMATCH));
    // A bodyless method outside an interface
    CHECK(helpers::raised(R"(
        const W = interface { pub const wr = fn(&self): i32; };
        const F = struct { fd: i32 };
        impl W for F { pub const wr = fn(&self): i32; }
    )",
                          sema::error::FUNCTION_DECLARATION_MISSING_BODY));
    // An import alias clashing with a parameter
    helpers::expect_compile_error(R"(
        const Ident = fn(T: type): type { import T; };
        pub const main = fn(): i32 { if (Ident(u8) == u8) { return 42; } return 0; };
    )");
    // A constant written to while its own initializer is folding
    helpers::expect_compile_error(
        "const pick = fn(x: i32): i32 { b = 0; return 1; }; const b = pick(1);");
    // Scoped nodes under an initializer's type or a runtime loop inside `for comptime`
    helpers::expect_compile_error(
        "pub const main = fn(): i32 { let x = match (1) { 1 => 1, _ => 2, }{ }; return 0; };");
    helpers::expect_compile_error(
        "const use = fn(): void { for comptime (0..3) |vv| { for (0..v) |j| { _ = j; } } };");
}

TEST_CASE("scoped expressions work as member-access objects and initializer types") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { a: i32, pub const g = fn(&self): i32 { return self.a; }; };
        const Box = fn(_: type): type { return struct { val: i32 }; };
        pub const main = fn(): i32 {
            let v = (blk: { break :blk S{ .a = 3 }; }).a;
            let w = (match (1) { 1 => S{ .a = 4 }, _ => S{ .a = 5 }, }).a;
            let x = (blk: { break :blk S; }){ .a = 7 };
            let mut b: Box(i32) = .{ .val = 1 };
            return v + w + x.a + S{ .a = 6 }.g() + b.val;
        };
    )") == 21);
}

TEST_CASE("bit reinterpretation needs a defined bit layout") {
    CHECK(helpers::raised(R"(
        const Pair = struct { a: u3, b: u5 };
        pub const main = fn(): i32 {
            let p: Pair = .{ .a = 5, .b = 5 };
            return @as(i32, @bitCast(u8, p));
        };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(R"(
        const Pair = packed struct { a: u3, b: u5 };
        pub const main = fn(): i32 {
            let p: Pair = .{ .a = 5, .b = 5 };
            return @as(i32, @bitCast(u16, p));
        };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::compile_and_run(R"(
        const Pair = packed struct { a: u3, b: u5 };
        pub const main = fn(): i32 {
            let p: Pair = .{ .a = 5, .b = 5 };
            return @as(i32, @bitCast(u8, p));
        };
    )") == 45);
}

TEST_CASE("a packed field write is type-checked like any store") {
    helpers::expect_compile_error(R"(
        const Flags = packed struct { on: bool, n: u7 };
        pub const main = fn(): i32 {
            let mut f: Flags = .{ .on = true, .n = 3 };
            f.on = if (f.n == 3) { return 4; };
            return 0;
        };
    )");
}

TEST_CASE("an uncalled builtin cannot be a type argument") {
    CHECK(helpers::raised(R"(
        const Box = fn(T: type): type { return struct { v: T }; };
        const B = Box(@sizeOf);
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a type constructor must return its type") {
    CHECK(helpers::raised(R"(
        const Vec = fn(T: type): type { struct { item: T }; };
        pub const main = fn(): i32 { const a: Vec(i32) = undefined; return 0; };
    )",
                          sema::error::RETURN_TYPE_MISMATCH));
}

TEST_CASE("scalar match patterns must fit the matched type") {
    const auto with_pattern{[](std::string_view matched, std::string_view pattern) {
        return fmt::format("pub const main = fn(): i32 {{ let mut n: {} = undefined; "
                           "return match (n) {{ {} => 1, _ => 0, }}; }};",
                           matched,
                           pattern);
    }};
    CHECK(helpers::raised(with_pattern("i32", "0.8"), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::raised(with_pattern("i32", "true"), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::raised(with_pattern("i32", "\"x\""), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::raised(with_pattern("bool", "3"), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::raised(with_pattern("u8", "300"), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::raised(with_pattern("u8", "0..300"), sema::error::ILLEGAL_MATCH_PATTERN));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: u8 = 3;
            let k: u8 = 3;
            return match (n) { 0, 2, 4..8 => 1, k => 7, 255 => 2, _ => 0, };
        };
    )") == 7);
}

TEST_CASE("a repeated self-reference reports once instead of crashing") {
    CHECK(helpers::raised(
        "const f = fn(rest...): void {}; const use = fn(): void { let g = f(g, g); };",
        sema::error::CYCLIC_DEPENDENCY));
    helpers::expect_compile_error(R"(
        const VTable = struct { step: fn(n: i32): i32 };
        const Widget = struct {
            const table = VTable{ .step = inc };
            const inc = fn(n: i32): i32 { let vt = ^Widget.table; return vt.step(n); };
        };
        pub const main = fn(): i32 { return Widget.inc(1); };
    )");
}

TEST_CASE("a type cannot be an index or an asm input") {
    CHECK(helpers::raised(
        "pub const main = fn(): i32 { let a: [3]i32 = .{1, 2, 3}; return a[i32]; };",
        sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised(R"(
        pub const f = fn(): void {
            asm { template: "syscall", inputs: ("{rax}" = i64), options: (volatile) };
        };
    )",
                          sema::error::ILLEGAL_INLINE_ASM));
}

TEST_CASE("a struct field is only reachable through an instance") {
    CHECK(helpers::raised(
        "const C = struct { value: i32 }; pub const main = fn(): i32 { return C.value; };",
        sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised(R"(
        const C = struct { value: i32 };
        pub const main = fn(): i32 { const c = C; return c.value; };
    )",
                          sema::error::TYPE_USED_AS_VALUE));
}

TEST_CASE("a type with no namespace has no members to reach through it") {
    CHECK(helpers::raised(R"(
        const C = struct { value: i32 };
        pub const main = fn(): i32 { const c = fn(): C; return c.value; };
    )",
                          sema::error::TYPE_USED_AS_VALUE));
    CHECK(helpers::raised("const A = [3]i32; pub const main = fn(): usize { return A.len; };",
                          sema::error::TYPE_USED_AS_VALUE));
}

TEST_CASE("a closure only modifies a captured `let mut`") {
    CHECK(helpers::raised(R"(
        const outer = fn(): void {
            let offset: i32 = 0;
            let add = fn(x: i32): void { offset = x; };
            add(1);
        };
    )",
                          sema::error::ASSIGNMENT_TO_CONST));
    CHECK(helpers::raised("const f = fn(x: i32): void { let g = fn(): void { x = 1; }; g(); };",
                          sema::error::ASSIGNMENT_TO_CONST));
    CHECK(helpers::raised(
        "const f = fn(): void { for (0..3) |i| { let g = fn(): void { i = 1; }; g(); } };",
        sema::error::ASSIGNMENT_TO_CONST));
    // Writing through an immutable slice binding only reads the binding
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut buf: [3]mut u8 = .{ 0, 0, 0 };
            let s: []mut u8 = buf[0..3];
            let set = fn(i: usize, v: u8): void { s[i] = v; };
            set(1, 7);
            return @intCast(i32, buf[1]);
        };
    )") == 7);
}

TEST_CASE("a range nested inside a `for` iterable or subscript is not a value") {
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 2;
            for (n = -1..n * 3) |i| { n = i; }
            return n;
        };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
    CHECK(helpers::raised(
        "pub const main = fn(): void { for (blk: { break :blk 0..1; }) |i| { _ = i; } };",
        sema::error::ILLEGAL_OPEN_RANGE));
    CHECK(helpers::raised("pub const f = fn(a: []i32): void { _ = a[blk: { break :blk 0..1; }]; };",
                          sema::error::ILLEGAL_OPEN_RANGE));
}

TEST_CASE("a generic function cannot be C-variadic") {
    CHECK(helpers::raised(R"(
        const first = fn(a: auto, b: auto, ...): auto { return a; };
        pub const main = fn(): i32 { return first(1, 2, 3); };
    )",
                          sema::error::MALFORMED_PACK_USE));
}

TEST_CASE("match patterns missing a comma are an error, not a member access") {
    CHECK(helpers::raised(R"(
        const E = enum { a, b };
        pub const main = fn(): i32 { let e: E = .a; return match (e) { .a .b => 1, _ => 2 }; };
    )",
                          sema::error::ILLEGAL_MATCH_PATTERN));
}

TEST_CASE("a `deprecated` message that isn't a string is reported, not a crash at its use") {
    CHECK(helpers::raised(R"(
        @[deprecated(-1)] const stale = fn(): i32 { return 1; };
        pub const main = fn(): i32 { return stale(); };
    )",
                          sema::error::ILLEGAL_ATTRIBUTE));
}

TEST_CASE("an array literal of `noreturn` elements is an error, not a crash") {
    CHECK(helpers::raised(R"(
        const ARR = [3]noreturn{7, 8, 9};
        pub const main = fn(): i32 { let i: usize = 2; _ = ARR[i]; return 0; };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a member of a method reference is an error, not its return type's member") {
    CHECK(helpers::raised(R"(
        const S = struct { item: i32, const make = fn(v: i32): S { return .{ .item = v }; }; };
        pub const main = fn(): i32 { let a = S.make; return a.item + 1; };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("a `dyn` method that isn't called is an error, not a crash") {
    CHECK(helpers::raised(R"(
        const Vec = interface { pub const x = fn(&self): i32; };
        const P = struct { a: i32 };
        impl Vec for P { pub const x = fn(&self): i32 { return self.a; }; }
        const norm = fn(v: &dyn Vec): i32 { return v.x + 1; };
        pub const main = fn(): i32 { let p = P{ .a = 4 }; return norm(&p); };
    )",
                          sema::error::TYPE_MISMATCH));
}

} // namespace ghoti::tests
