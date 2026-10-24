#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "compiler/sema/error.hh"
#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

// Inputs the mutation fuzzer turned into compiler crashes; each must now be a clean diagnostic
namespace ghoti::tests {

TEST_CASE("An attribute argument with a builtin arity error is reported, not a crash") {
    helpers::expect_compile_error(R"(
        const misaligned = fn(comptime T: type): i32 {
            @[align(if (@sizeOf() > 4) 64 else 32)]
            let mut buf: [4]u8 = undefined;
            _ = buf;
            return 0;
        };
        pub const main = fn(): i32 { return misaligned(i32); };
    )");
}

TEST_CASE("@hasField rejects a non-type owner and a non-string name") {
    helpers::expect_compile_error(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 { return @intFromBool(@hasField(Point, Point)); };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { return @intFromBool(@hasField("x", "z")); };
    )");
}

TEST_CASE("An untyped float can't be returned from an integer function") {
    helpers::expect_compile_error("pub const main = fn(): i32 { return 1.5; };");
}

TEST_CASE("Unary operators reject a type operand") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let x = -i64;
            _ = x;
            return 0;
        };
    )");
}

TEST_CASE("A pack parameter named as a return type is an error, not a crash") {
    helpers::expect_compile_error(R"(
        const g = fn(rest...): rest {};
        const f = fn(void...): void { g(rest...); };
        const use = fn(): void { f(1, 2, 3); };
    )");
}

TEST_CASE("Cast builtins reject an undefined operand") {
    helpers::expect_compile_error(R"(
        const f = fn(x: i32): u32 {
            let mut a: u32 = @bitCast(x);
            a = @bitCast(undefined);
            return a;
        };
    )");
}

TEST_CASE("Code after a match whose arms all return is dead, not miscompiled") {
    CHECK(helpers::compile_and_run(R"(
        const pick = fn(n: i32): i32 {
            let v = match (n) {
                _ => { return 99; },
            };
            return v + 1;
        };
        pub const main = fn(): i32 { return pick(5); };
    )") == 99);
}

TEST_CASE("A field can't have type noreturn") {
    helpers::expect_compile_error(R"(
        const S = struct {
            a: i32,
            @cfg(ptr_bits == 7) { linux_only: i32 }
            else                { fallback: noreturn }
            z: i32,
        };
        let mut s: S = undefined;
    )");
    helpers::expect_compile_error("let mut x: [2]noreturn = undefined;");
}

TEST_CASE("An unrolled loop over a condition-less if comptime is checked without crashing") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll loop {
                if comptime fn(): i32 (n == 5) { break; }
                n = n + 1;
            }
            return n;
        };
    )");
}

TEST_CASE("Match arms yielding a type beside arms yielding values are an error") {
    helpers::expect_compile_error(R"(
        const P = struct { a: i32 };
        const f = fn(n: i32): P {
            return match (n) {
                0 => P,
                _ => P{ .a = 1 },
            };
        };
    )");
}

TEST_CASE("A type whose layout depends on its own size or alignment is an error") {
    CHECK(helpers::raised(R"(
        const S = struct { pub d: [N]u64, pub x: u8, };
        const N: [@alignOf(S)]u8 = undefined;
    )",
                          sema::error::COMPTIME_EVALUATION_FAILED));
}

TEST_CASE("An array's `.len` and `.ptr` can't be assigned") {
    CHECK(helpers::raised(
        "pub const main = fn(): i32 { let mut a: [4]u8 = undefined; a.len = 3; return 0; };",
        sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(R"(
        const size: [4]u8 = undefined;
        pub const main = fn(): i32 { if (size.len = 32) { return 1; } return 0; };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("A block can't be indexed like a value") {
    helpers::test_parser_fail(
        "const f = fn(n: i32): i32 { if (n != 3) { return -1; }[1]; return 0; };",
        syntax::diagnostic{
            "Expected an expression, found ';'", syntax::error::MISSING_PREFIX_PARSER, 0, 57});
}

TEST_CASE("@cfg inside an if nested in an operand is expanded") {
    helpers::expect_compile_error(R"(
        pub const f = fn(): i32 {
            let b = 1 >= if (true) {
                @cfg(ptr_bits >= 8) { @compileError("reached in if"); }
            };
            return 0;
        };
    )");
}

TEST_CASE("The `type` keyword and a type argument for a generic `[]T` slot are not values") {
    helpers::expect_compile_error(
        "pub const main = fn(): i32 { let s: [:0]u8 = type; return 0; };");
    helpers::expect_compile_error(R"(
        pub const f = fn(comptime T: type, a: []T): usize { return a.len; };
        pub const main = fn(): i32 { return @intCast(i32, f(u8, noreturn)); };
    )");
}

TEST_CASE("A type declaration has no storage, however large the type") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Big = struct { data: [100000100000]mut u8 };
        const E = enum { a, b };
        pub const size = fn(): usize { return @sizeOf(Big); };
    )")};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK(llvm_mod->getNamedGlobal("Big") == nullptr);
    CHECK(llvm_mod->getNamedGlobal("E") == nullptr);
}

TEST_CASE("A type is not a range bound, an asm input, or a converted operand") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut a = [5uz]mut u8{ 1, 2, 3, 4, 5 };
            @memmove(a[1..5], a[u8..4]);
            return 0;
        };
    )");
    helpers::expect_compile_error(R"(
        pub const f = fn(fd: i64): i64 {
            let mut ret: i64 = 0i64;
            asm {
                template: "syscall",
                outputs: ("={rax}" = ret),
                inputs: ("{rax}" = 1i64, "{rdi}" = fd, "{rsi}" = type),
            };
            return ret;
        };
    )");
    helpers::expect_compile_error(R"(
        const Color = enum : i32 { red, green = 5, blue };
        pub const main = fn(): i32 { return @backingInt(Color); };
    )");
}

TEST_CASE("Atomic builtins reject a value for `T` and a type for an operand") {
    helpers::expect_compile_error(R"(
        pub const f = fn(p: ^mut i32): i32 {
            return @atomicRmw(1, p, builtin.AtomicRmwOp.add, i32, builtin.MemoryOrder.seq_cst);
        };
    )");
    helpers::expect_compile_error(R"(
        pub const f = fn(p: ^mut i32): i32 {
            return @atomicRmw(i32, p, builtin.AtomicRmwOp.add, i32, builtin.MemoryOrder.seq_cst);
        };
    )");
}

TEST_CASE("@cVaArg reads a concrete value type") {
    helpers::expect_compile_error(R"(
        const f = fn(ap: ^mut opaque, ...): void {
            let val: i32 = @cVaArg(ap, impl i32);
            _ = val;
        };
    )");
    helpers::expect_compile_error(R"(
        const f = fn(ap: ^mut opaque, ...): void {
            let val: i32 = @cVaArg(ap, 3);
            _ = val;
        };
    )");
}

TEST_CASE("A binary operator can't read `undefined`") {
    helpers::expect_compile_error(R"(
        const N = 2;
        pub const main = fn(): i32 {
            match (N) {
                2 => @assert(undefined == 3),
                _ => {},
            }
            return 0;
        };
    )");
}

TEST_CASE("A module-level initializer must match its array annotation") {
    helpers::expect_compile_error(R"(
        const P: [0]u8 = "ABC";
        pub const main = fn(): i32 { return 0; };
    )");
    helpers::expect_compile_error(R"(
        const P: [3]u8 = "ABC";
        pub const main = fn(): i32 { return @as(i32, P[0]); };
    )");
    helpers::expect_compile_error(R"(
        const P: [3]i64 = [3]i32{ 1, 2, 3 };
        pub const main = fn(): i32 { return 0; };
    )");
    helpers::expect_compile_error(R"(
        const table: [4]i32 = fn(arr: [4]i32, i: usize): i32 { return arr[i]; };
    )");
    CHECK(helpers::compile_and_run(R"(
        const P: [3:0]u8 = "ABC";
        pub const main = fn(): i32 { return @as(i32, P[0]); };
    )") == 65);
}

TEST_CASE("A function literal in an indexed call's arguments gets its scope") {
    helpers::expect_compile_error(R"(
        const map = fn(func: fn(x: i32): i32): void {};
        pub const main = fn(): i32 {
            map(fn(x: i32): i32 { return x; })[0];
            return 0;
        };
    )");
    CHECK(helpers::compile_and_run(R"(
        const map = fn(func: fn(x: i32): i32): [2]i32 { return .{ func(1), func(2) }; };
        pub const main = fn(): i32 {
            let offset: i32 = 10;
            return map(fn(x: i32): i32 { return x + offset; })[1];
        };
    )") == 12);
}

TEST_CASE("A folded call checks a number passed for an array parameter") {
    helpers::expect_compile_error(R"(
        const chain = fn(a: auto, b: @TypeOf("s")): @TypeOf(b) { return b; };
        const call_chain = chain(1, 2);
    )");
    helpers::expect_compile_error(R"(
        const first = fn(b: [1:0]u8): i32 { return 0; };
        const result = first(2);
    )");
}

TEST_CASE("A match on an enum literal or a wide float is reported, not a crash") {
    helpers::expect_compile_error(R"(
        const K = enum { a, b };
        const g = fn(k: impl K, x: match (k) { .a => u8, .b => u64 }): usize { _ = x; return 1; };
        pub const main = fn(): i32 { return @intCast(g(.a, 1)); };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            const x: f16 = 1.0;
            return match (x) { _ => 0 };
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            const x: f128 = 1.0;
            return match (x) { _ => 0 };
        };
    )");
}

TEST_CASE("A @cfg block in a function signature is spliced before symbol collection") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): comptime {
            @cfg(ptr_bits == 64) { return 64; }
            else                 { return 32; }
        };
    )");
}

TEST_CASE("An enum member reached from its own body before it resolves is not a crash") {
    helpers::expect_compile_error(R"(
        pub const Color = enum {
            RED,
            pub const get_default = fn(): Color { return Color.secret_code = fn(): i32RED; };
            const secret_code = fn(): i32 { return 42; };
        };
        pub const main = fn(): i32 { return 0; };
    )");
}

TEST_CASE("A module-scope initializer of the wrong type is rejected") {
    helpers::expect_compile_error(R"(
        let mut current: fn(n: i32): i32 = "s";
        pub const main = fn(): i32 { return current(0); };
    )");
    helpers::expect_compile_error(R"(
        let mut current: i32 = "s";
        pub const main = fn(): i32 { return current; };
    )");
    helpers::expect_compile_error(R"(
        const current: bool = "s";
        pub const main = fn(): i32 { return @intFromBool(current); };
    )");
    CHECK(helpers::compile_and_run(R"(
        const name: []u8 = "abc";
        pub const main = fn(): i32 { return @intCast(name.len); };
    )") == 3);
}

TEST_CASE("A folded call returning a reference parameter by value reads through it") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(y: &i64): i64 { return y; };
        const c = f(&2);
        pub const main = fn(): i32 { return @intCast(c); };
    )") == 2);
    CHECK(helpers::compile_and_run(R"(
        const echo = fn(x: auto, y: @TypeOf(auto)): @TypeOf(x) { return y; };
        const c = echo(1, &2);
        pub const main = fn(): i32 { return @intCast(c); };
    )") == 2);
    CHECK(helpers::compile_and_run(R"(
        const g: i64 = 5;
        const f = fn(y: &i64): i64 { return y; };
        const c = f(&g);
        pub const main = fn(): i32 { return @intCast(c); };
    )") == 5);
}

TEST_CASE("A reference to a function coerces to an erased fn parameter") {
    CHECK(helpers::compile_and_run(R"(
        const ap = fn(m: fn(x: i32): bool): bool { return m(1); };
        const is_pos = fn(x: i32): bool { return x > 0; };
        pub const main = fn(): i32 { return @intFromBool(ap(&is_pos)); };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        const ap = fn(comptime U: type, m: fn(x: i32): U): U { return m(4); };
        const twice = fn(x: i32): i64 { return x * 2; };
        pub const main = fn(): i32 { return @intCast(ap(i64, &twice)); };
    )") == 8);
}

TEST_CASE("A generic member taking self by value is typed for its instance") {
    CHECK(helpers::compile_and_run(R"(
        const Container = fn(comptime T: type): type {
            return struct {
                val: T,
                pub const get = fn(self, comptime U: type): U { return @intCast(U, self.val); };
            };
        };
        pub const main = fn(): i32 {
            let c: Container(i32) = .{ .val = 10 };
            return @intCast(c.get(i64));
        };
    )") == 10);
    CHECK(helpers::compile_and_run(R"(
        const Container = fn(comptime T: type): type { return struct { val: T }; };
        impl(T: type) Container(T) {
            pub const map = fn(self, comptime U: type, mapper: fn(x: T): U): Container(U) {
                return .{ .val = mapper(self.val) };
            };
        }
        pub const main = fn(): i32 {
            let c1: Container(i32) = .{ .val = 10 };
            const dbl = fn(x: i32): i64 { return @intCast(i64, x * 2); };
            const is_gt = fn(x: i32): bool { return x > 5; };
            let c2 = c1.map(i64, dbl);
            let c3 = c1.map(bool, &is_gt);
            return @intCast(i32, c2.val) + @intFromBool(c3.val);
        };
    )") == 21);
}

TEST_CASE("An enum backed by a non-integer type is rejected") {
    CHECK(helpers::raised("const E = enum : f32 { a, b }; pub const f = fn(e: E): void {};",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(
        "const E = enum : @Vector(2, u8) { a }; pub const f = fn(): u8 { const e: E = .a; "
        "return @backingInt(e)[0]; };",
        sema::error::TYPE_MISMATCH));
}

TEST_CASE("Vector builtins that reached LLVM with bad operands are diagnostics") {
    helpers::expect_compile_error(
        "pub const f = fn(a: @Vector(4, u8)): u16 { return @bitCast(a); };");
    helpers::expect_compile_error("pub const f = fn(): @Vector(4, u8) { return @splat(u8); };");
}

TEST_CASE("An enum literal with no enum to name has no address") {
    CHECK(
        helpers::raised("pub const f = fn(): void { let p = &.a; };", sema::error::TYPE_MISMATCH));
    CHECK(
        helpers::raised("pub const f = fn(): void { let p = ^.a; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("A shift of an integer wider than 128 bits folds without overflowing its storage") {
    CHECK(helpers::compile_and_run(R"(
        const top = fn(t: u256): u8 { return @truncate(u8, t >> 250); };
        const back = fn(t: i256): i256 { return t << 200 >> 199; };
        pub const main = fn(): i32 {
            let x: u256 = 3 << 250;
            let y: i256 = -1;
            return @as(i32, top(x)) + @as(i32, @truncate(i8, back(y)));
        };
    )") == 3 - 2);
}

TEST_CASE("A redundant `comptime` on a function literal is reported") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let pick = comptime fn(a: i32, b: i32): i32 { return a; };
            return pick(1, 2);
        };
    )");
}

TEST_CASE("A compile-time argument has to be its parameter's type") {
    CHECK(helpers::raised(R"(
        const H = fn(comptime x: f64): type { return struct { pub const b: u64 = @bitCast(x); }; };
        pub const f = fn(): u64 { return H(0..0).b; };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(R"(
        const H = fn(comptime x: f64): u64 { return @bitCast(x); };
        pub const f = fn(): u64 { return H(0..0); };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("A reference to an untyped integer call result is diagnosed") {
    helpers::expect_compile_error(R"(
        pub const max_of = fn(comptime T: type): comptime_int {
            return (1 << @typeInfo(T).int.bits) - 1;
        };
        pub const main = fn(): i32 { return @intCast((&max_of(u32)) - 7); };
    )");
}

TEST_CASE("A global's folded initializer still has to fit its annotation element by element") {
    CHECK(helpers::raised(R"(
        const P: [:0]type = "ABC";
        pub const main = fn(): i32 { let mut n: i32 = 0; for (P) |c| { n += @as(i32, c); } return n; };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("pub const cases = [_][3]u32{ [_]u32{ 10, 20, 60, 30 } };",
                          sema::error::TYPE_MISMATCH));
    // A string is `u8`s, which a folded constant would reinterpret rather than widen
    CHECK(helpers::raised(R"(
        const P: [:0]u16 = "ABC";
        pub const main = fn(): i32 { const L: [:0]u16 = "hi"; return @as(i32, P[1] + L[1]); };
    )",
                          sema::error::TYPE_MISMATCH));
    // Known numbers narrow into the elements, range-checked as they fold
    CHECK(helpers::compile_and_run(R"(
        const W: usize = 10;
        const Q = [_][2]u8{ [_]u8{ W, 2 } };
        pub const main = fn(): i32 { return @as(i32, Q[0][0]); };
    )") == 10);
}

TEST_CASE("A number can't take a pointer in a compound assignment") {
    CHECK(helpers::raised(
        "pub const f = fn(): i32 { let mut c: i32 = 0; let y: i32 = 5; c += ^y; return c; };",
        sema::error::OPERATOR_TYPE_MISMATCH));
    // A reference reads as its value, and a pointer still steps
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut c: i32 = 10;
            let y: i32 = 3;
            c -= &y;
            let a: [3]u8 = .{ 1, 2, 3 };
            let mut p: ^u8 = ^a[0];
            p += 2;
            return c + *p;
        };
    )") == 7 + 3);
}

TEST_CASE("An array sized by a runtime value is an error, not a local without storage") {
    const auto runtime_length{[](std::string_view source) {
        const auto diags{helpers::type_check_diags(source)};
        CHECK(diags.message_contains("An array's length must be known at compile time"));
    }};
    runtime_length(R"(
        pub const f = fn(): u8 {
            let mut v: u8 = 3;
            let mut arr: [v]mut u8 = .{ 4, 5, 6 };
            let s = arr[0..];
            return s[0];
        };
    )");
    runtime_length(
        "pub const f = fn(k: usize): void { let mut v = k; let arr: [v]u8 = undefined; };");
}

TEST_CASE("Math builtins take values, not types") {
    for (const auto* body : {"return @clz(u8);", "return @min(u8, 3);", "return @abs(i32);"}) {
        CHECK(helpers::raised(fmt::format("pub const f = fn(): i32 {{ {} }};", body),
                              sema::error::TYPE_USED_AS_VALUE));
    }
}

} // namespace ghoti::tests
