#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@Vector lays out as LLVM's vector type") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, f32);
        pub const main = fn(): i32 {
            comptime {
                @assert(@sizeOf(V) == 16);
                @assert(@alignOf(V) == 16);
                @assert(@sizeOf(@Vector(3, f32)) == 16);
                @assert(@sizeOf(@Vector(8, bool)) == 1);
                @assert(@sizeOf(@Vector(4, i7)) == 4);
                @assert(@sizeOf(@Vector(4, u64)) == 32);
                @assert(@typeInfo(V).vector.len == 4);
                @assert(@typeInfo(V).vector.child == f32);
                @assert(@Vector(.{ .len = 4, .child = f32 }) == V);
                @assert(@Vector(2 + 2, f32) == V);
            }
            return 42;
        };
    )") == 42);
}

TEST_CASE("Vector operators work per lane and broadcast a scalar") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, i32);
        pub const main = fn(): i32 {
            let a: V = .{ 1, 2, 3, 4 };
            let b: V = .{ 10, 20, 30, 40 };
            let c = (a + b) * 2;
            let d = 1 + c;
            return d[0] + d[3];
        };
    )") == 23 + 89);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: @Vector(4, f32) = .{ 1.5, 2.5, 3.5, 4.5 };
            let b = a * 2.0 - 1.0;
            let s: u16 = 3;
            let w: @Vector(4, u32) = @as(@Vector(4, u32), .{ 1, 2, 3, 4 }) + s;
            return if (b[0] == 2.0 and b[3] == 8.0 and w[0] == 4) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vector comparisons give bool vectors, which combine per lane") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let x: @Vector(4, i32) = .{ 1, 5, 3, 7 };
            let big = x > 2;
            let odd = x == .{ 1, 5, 0, 0 };
            let both = big and odd;
            let either = big or odd;
            let none = !either;
            return if (both[1] and !both[0] and either[0] and either[2] and !none[3]) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vector unary, bitwise, shift, wrapping, and saturating operators") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: @Vector(4, i32) = .{ 1, -2, 4, -8 };
            let n = -a;
            let m = ~a;
            let s = a << 2;
            let r = a >> .{ 0, 1, 2, 3 };
            let x = (a & 6) | (a ^ 1);
            let ok = n[1] == 2 and m[0] == -2 and s[2] == 16 and r[3] == -1 and x[0] == 0;
            return if (ok) 42; else 0;
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: @Vector(4, u8) = .{ 250, 10, 0, 1 };
            let w = a +% 10;
            let s = a +| 10;
            let neg = -%a;
            return if (w[0] == 4 and s[0] == 255 and s[1] == 20 and neg[3] == 255) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vector lanes are read and written by index") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            a[2] = 30;
            a[0] += 9;
            let mut i: usize = 3;
            a[i] *= 10;
            return a[0] + a[2] + a[i];
        };
    )") == 10 + 30 + 40);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut v: @Vector(8, bool) = undefined;
            let mut i: usize = 0;
            while (i < 8) : (i += 1) { v[i] = i % 2 == 0; }
            return if (v[0] and !v[1] and v[6]) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Arrays and vectors convert into each other by value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let arr: [4]i32 = .{ 5, 6, 7, 8 };
            let v: @Vector(4, i32) = arr;
            let back: [4]i32 = v + 1;
            return back[0] + back[3];
        };
    )") == 6 + 9);
}

TEST_CASE("Vectors pass, return, and live in aggregates, pointers, and arrays") {
    CHECK(helpers::compile_and_run(R"(
        const F4 = @Vector(4, f32);
        const S = struct { v: F4, x: i32 };
        const twice = fn(v: F4): F4 { return v + v; };
        const bump = fn(p: ^mut F4): void { *p = *p * 2.0; };
        const pick = fn(c: bool, a: F4, b: F4): F4 { return if (c) a; else b; };
        pub const main = fn(): i32 {
            let mut v: F4 = .{ 1.0, 2.0, 3.0, 4.0 };
            bump(^mut v);
            let s: S = .{ .v = twice(v), .x = 3 };
            let mut arr: [2]mut F4 = .{ v, v };
            arr[1] = arr[0] + arr[1];
            let mut c = false;
            let ok = s.v[3] == 16.0 and @sizeOf(S) == 32 and arr[1][3] == 16.0 and
                     pick(c, v, s.v)[0] == 4.0;
            return if (ok) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vectors take every lane type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let h: @Vector(4, f16) = .{ 1.5, 2.5, 3.5, 4.5 };
            let odd: @Vector(4, i7) = .{ 1, 2, 3, -4 };
            let wide: @Vector(2, u128) = .{ 1, 2 };
            let a: i32 = 7;
            let b: i32 = 9;
            let ptrs: @Vector(2, ^i32) = .{ ^a, ^b };
            let ok = (h + h)[0] == 3.0 and (odd * 2)[3] == -8 and
                     (wide << 100)[1] == (@as(u128, 2) << 100) and *ptrs[1] == 9;
            return if (ok) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vectors bit-cast to and from integers of their width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let v: @Vector(4, u8) = .{ 1, 2, 3, 4 };
            let n = @bitCast(u32, v);
            let back = @bitCast(@Vector(4, u8), n);
            return if (n == 0x04030201 and back[2] == 3) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("Vector operations fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const G: @Vector(4, i32) = .{ 7, 8, 9, 10 };
        pub const main = fn(): i32 {
            const k: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            const j = k * k + G;
            const m = -k;
            comptime {
                @assert(j[3] == 26);
                @assert(m[1] == -2);
                @assert((k > 2)[2]);
            }
            return G[1] + j[0];
        };
    )") == 8 + 8);
}

TEST_CASE("Vectors in generic signatures") {
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(comptime T: type, v: @Vector(4, T)): @Vector(4, T) { return v + v; };
        const first = fn(v: auto): i32 { return v[0]; };
        pub const main = fn(): i32 {
            let v: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            return twice(i32, v)[3] + first(v);
        };
    )") == 8 + 1);
}

TEST_CASE("@llvmIntrinsic takes and returns vectors") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let v: @Vector(4, u32) = .{ 1, 2, 3, 0xff };
            let p = @llvmIntrinsic("llvm.ctpop", @Vector(4, u32), v);
            return @intCast(i32, p[3] + p[2]);
        };
    )") == 8 + 2);
}

TEST_CASE("Vector arithmetic checks every lane under runtime safety", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: @Vector(4, i32) = .{ 1, 2147483647, 1, 1 };
            let b = a + 1;
            return b[0];
        };
    )") != 2);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut d: @Vector(4, i32) = .{ 1, 1, 0, 1 };
            let b = @as(@Vector(4, i32), .{ 8, 8, 8, 8 }) / d;
            return b[0];
        };
    )") != 8);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let mut i: usize = 7;
            return a[i];
        };
    )") > 4);
}

TEST_CASE("@Vector rejects lanes and lengths it can't hold") {
    const auto lane{helpers::type_check_diags(
        "pub const f = fn(): void { let a: @Vector(4, f80) = undefined; };")};
    CHECK(lane.message_contains("'f80' can't be a '@Vector' lane"));

    const auto aggregate{helpers::type_check_diags(
        "pub const f = fn(): void { let a: @Vector(4, []u8) = undefined; };")};
    CHECK(aggregate.message_contains("lanes are integers, floats, 'bool', or pointers"));

    const auto empty{helpers::type_check_diags(
        "pub const f = fn(): void { let a: @Vector(0, f32) = undefined; };")};
    CHECK(empty.message_contains("'@Vector' has 0 lanes"));

    // A lane count makes the parameter it reads compile-time, as an array length does
    const auto unknown{helpers::type_check_diags(R"(
        const f = fn(n: usize): i32 { let a: @Vector(n, i32) = undefined; return a[0]; };
        pub const g = fn(): i32 { let mut k: usize = 4; k += 1; return f(k); };
    )")};
    CHECK(unknown.message_contains("'n' is compile-time because the function reads it"));
}

TEST_CASE("Vector operators reject mismatched shapes and lanes") {
    const auto shapes{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let b: @Vector(2, i32) = .{ 1, 2 };
            let c = a + b;
        };
    )")};
    CHECK(shapes.message_contains(
        "Operator '+' cannot be applied to types '@Vector(4, i32)' and '@Vector(2, i32)'"));

    const auto broadcast{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let c = a * 2.5;
        };
    )")};
    CHECK(broadcast.message_contains(
        "'comptime_float' does not broadcast to the lanes of '@Vector(4, i32)'"));

    const auto logical{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let c = a and a;
        };
    )")};
    CHECK(logical.message_contains("operator 'and' needs 'bool' lanes"));

    const auto shift{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let c = a << 40;
        };
    )")};
    CHECK(shift.message_contains("Shift amount is not less than the 32-bit width"));
}

TEST_CASE("Vector lanes have no address, and a vector can't be sliced") {
    const auto address{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let p = ^a[0];
        };
    )")};
    CHECK(address.codes.front() == sema::error::ILLEGAL_VECTOR_LANE_ADDRESS);

    const auto slice{helpers::type_check_diags(R"(
        pub const f = fn(): void {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let s = a[0..2];
        };
    )")};
    CHECK(slice.message_contains("A vector can't be sliced"));

    const auto bounds{helpers::type_check_diags(R"(
        pub const f = fn(): i32 {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            return a[4];
        };
    )")};
    CHECK(bounds.message_contains("Index 4 is out of bounds for a length of 4"));
}

TEST_CASE("Vector initializers take one value per lane") {
    const auto res{helpers::type_check_diags(
        "pub const f = fn(): void { let a: @Vector(4, i32) = .{ 1, 2, 3 }; };")};
    CHECK(res.message_contains("Vector of length 4 initialized with 3 value(s)"));
}

TEST_CASE("A packed aggregate holds no vectors") {
    const auto res{helpers::type_check_diags("const P = packed struct { v: @Vector(4, u8) };")};
    CHECK(res.message_contains("store a vector's lanes as `[N]T` instead"));
}

TEST_CASE("An extern signature takes only vectors with a C ABI") {
    const auto bad{helpers::type_check_diags("extern const f: fn(v: @Vector(3, f32)): void;")};
    CHECK(bad.codes.front() == sema::error::ILLEGAL_EXTERN_VECTOR);
    CHECK(bad.message_contains("'@Vector(3, f32)' has no C ABI"));

    const auto field{helpers::type_check_diags("const E = extern struct { v: @Vector(2, f32) };")};
    CHECK(field.codes.front() == sema::error::ILLEGAL_EXTERN_VECTOR);

    CHECK(helpers::type_check_diags("extern const g: fn(v: @Vector(4, f32)): @Vector(8, f32);")
              .codes.empty());
}

TEST_CASE("A signature's vector length can come from a compile-time parameter") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(comptime n: usize, v: @Vector(n, i32)): i32 { return v[0] + v[n - 1]; };
        const twice = fn(n: usize, v: @Vector(n, i32)): @Vector(n, i32) { return v * 2; };
        const widen = fn(comptime w: u16, x: u8): @Int(.{ .signedness = .unsigned, .bits = w }) {
            return x;
        };
        pub const main = fn(): i32 {
            let v: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let w: u32 = widen(32, 7);
            return sum(4, v) + twice(4, v)[3] + @as(i32, @intCast(w));
        };
    )") == 5 + 8 + 7);

    CHECK(helpers::compile_and_run(R"(
        const Box = fn(comptime n: usize): type { return struct { items: [n]i32 }; };
        const last = fn(comptime n: usize, b: Box(n)): i32 { return b.items[n - 1]; };
        pub const main = fn(): i32 {
            let b: Box(3) = .{ .items = .{ 5, 6, 7 } };
            return last(3, b);
        };
    )") == 7);

    // An initializer builds the vector the earlier `comptime` argument sizes
    CHECK(helpers::compile_and_run(R"(
        const last = fn(comptime n: usize, v: @Vector(n, i32)): i32 { return v[n - 1]; };
        const twice = fn(comptime n: usize, v: @Vector(n, i32)): @Vector(n, i32) { return v * 2; };
        pub const main = fn(): i32 { return last(3, .{ 1, 2, 3 }) + twice(2, .{ 5, 6 })[1]; };
    )") == 3 + 12);

    const auto mismatch{helpers::type_check_diags(R"(
        const sum = fn(comptime n: usize, v: @Vector(n, i32)): i32 { return v[0]; };
        pub const f = fn(): i32 { let v: @Vector(2, i32) = .{ 1, 2 }; return sum(4, v); };
    )")};
    CHECK(mismatch.message_contains("Argument 2 of type '@Vector(2, i32)' is not assignable to "
                                    "parameter type '@Vector(4, i32)' in call to 'sum'"));

    // A `comptime` parameter before it still counts toward the argument's position
    const auto array_mismatch{helpers::type_check_diags(R"(
        const pick = fn(comptime n: usize, comptime k: usize, a: [n]i32, b: bool): i32 {
            return if (b) a[k]; else 0;
        };
        pub const f = fn(): i32 { let a: [3]i32 = .{ 1, 2, 3 }; return pick(3, 1, a, 7); };
    )")};
    CHECK(array_mismatch.message_contains("Argument 4 of type"));
    CHECK(array_mismatch.message_contains("in call to 'pick'"));
}

TEST_CASE("@splat, @reduce, @shuffle, and @select work on lanes") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, i32);
        const sum = fn(v: V): i32 { return @reduce(.add, v); };
        pub const main = fn(): i32 {
            let mut x: i32 = 3;
            x += 0;
            let s: V = @splat(x);
            let a: V = .{ 1, 2, 3, 4 };
            let b: V = .{ 10, 20, 30, 40 };
            let sh = @shuffle(a, b, .{ 3, -1, 0, -4 });
            let rev = @shuffle(a, undefined, .{ 3, 2, 1, 0 });
            let pick = @select(a > 2, a, b);
            let clamp = @select(a > 2, 0, a);
            let f: @Vector(4, f32) = .{ 1.5, -2.0, 8.0, 0.5 };
            let ok = @reduce(.max, f) == 8.0 and @reduce(.min, f) == -2.0 and
                @reduce(.@"or", a == 3) and !@reduce(.@"and", a == 3);
            let total = sum(s) + sum(sh) + rev[0] + sum(pick) + sum(clamp);
            return if (ok) total; else 0;
        };
    )") == 12 + 55 + 4 + 37 + 3);
}

TEST_CASE("Lane builtins fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, i32);
        pub const main = fn(): i32 {
            comptime {
                const a: V = .{ 1, 2, 3, 4 };
                @assert(@reduce(.mul, a) == 24);
                @assert(@reduce(.xor, a) == 4);
                @assert(@reduce(.add, @as(@Vector(2, u8), .{ 200, 100 })) == 44);
                @assert(@shuffle(a, @as(V, .{ 5, 6, 7, 8 }), .{ 0, -2 })[1] == 6);
                @assert(@select(a > 2, a, @as(V, @splat(9)))[0] == 9);
                const s: @Vector(3, f32) = @splat(1.5);
                @assert(@reduce(.add, s) == 4.5);
            }
            return 42;
        };
    )") == 42);
}

TEST_CASE("Masked loads and stores, gathers, and scatters touch only the lanes the mask picks") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, i32);
        pub const main = fn(): i32 {
            let buf: [6]mut i32 = .{ 1, 2, 3, 4, 5, 6 };
            let mask: @Vector(4, bool) = .{ true, true, false, true };
            let loaded = @maskedLoad(^buf[1], mask, @as(V, @splat(-1)));
            @maskedStore(^mut buf[2], @as(V, .{ 10, 20, 30, 40 }), mask);
            let ptrs: @Vector(2, ^mut i32) = .{ ^mut buf[0], ^mut buf[5] };
            let g = @gather(ptrs, @as(@Vector(2, bool), @splat(true)), @as(@Vector(2, i32), @splat(0)));
            @scatter(@as(@Vector(2, i32), .{ 7, 8 }), ptrs, .{ false, true });
            return @reduce(.add, loaded) + buf[2] + buf[3] + buf[4] + g[1] + buf[5] + buf[0];
        };
    )") == 9 + 10 + 20 + 5 + 40 + 8 + 1);
}

TEST_CASE("Lane builtins reject operands that don't fit") {
    const auto check{[](std::string_view body, std::string_view needle) {
        const auto diags{helpers::type_check_diags(
            fmt::format("pub const f = fn(): void {{ let a: @Vector(4, i32) = .{{ 1, 2, 3, 4 }}; "
                        "let fl: @Vector(4, f32) = @splat(1.0); {} }};",
                        body))};
        CHECK(diags.message_contains(needle));
    }};
    check("let s = @splat(1);", "'@splat': needs a vector result type");
    check("let r = @reduce(.@\"and\", fl);", "'.and' does not reduce 'f32' lanes");
    check("let s = @shuffle(a, a, .{ 0, 4 });", "selects lane 4 of '@Vector(4, i32)', which has 4");
    check("let s = @shuffle(a, fl, .{ 0, -1 });", "expects a vector of 'i32' lanes");
    check("let m: @Vector(2, bool) = .{ true, false }; let s = @select(m, a, a);",
          "the mask must be a '@Vector(4, bool)'");
    check("let x: i32 = 1; let p = ^x; @maskedStore(p, a, a == 1);", "points to constant memory");
    check("let b: [4]bool = .{ true, true, true, true }; "
          "let v = @maskedLoad(^b[0], a == 1, a == 1);",
          "a 'bool' lane has no memory form");
}

TEST_CASE("Math builtins work per lane and broadcast a scalar operand") {
    CHECK(helpers::compile_and_run(R"(
        const V = @Vector(4, i32);
        const F = @Vector(4, f32);
        pub const main = fn(): i32 {
            let mut k: i32 = 0;
            k += 1;
            let a: V = .{ -3, 8, 15, -1 } + k - 1;
            let f: F = .{ 4.0, 9.0, 2.25, 0.5 };
            let r = @sqrt(f);
            let s = @sin(@as(F, @splat(0.0)));
            let fl = @floor(@as(F, .{ 1.5, -1.5, 2.0, 0.1 }));
            let mn = @min(a, 0);
            let mx = @max(a, @as(V, @splat(5)));
            let ab = @abs(a);
            let pc = @popCount(@as(@Vector(4, u8), .{ 1, 3, 7, 255 }));
            let md = @mod(a, 4);
            let fm = @mulAdd(F, f, 2.0, 1.0);
            let mut out: V = undefined;
            let ov = @addWithOverflow(a, @as(V, @splat(2147483647)), &mut out);
            let ok = r[1] == 3.0 and s[0] == 0.0 and fl[1] == -2.0 and ov[1] and !ov[0] and
                fm[2] == 5.5;
            let total = @reduce(.add, mn) + @reduce(.add, mx) + @reduce(.add, ab) +
                @as(i32, @intCast(@reduce(.add, pc))) + @reduce(.add, md);
            return if (ok) total; else 0;
        };
    )") == -4 + 33 + 27 + 14 + 7);
}

TEST_CASE("Numeric casts convert every lane") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut k: i32 = 0;
            k += 1;
            let a: @Vector(4, i32) = .{ 257, 2, -3, 4 } * k;
            let wide: @Vector(4, i64) = @intCast(a);
            let narrow: @Vector(4, u8) = @truncate(a);
            let f: @Vector(4, f32) = @floatFromInt(a);
            let d: @Vector(4, f64) = @floatCast(f);
            let back: @Vector(4, i16) = @intFromFloat(d);
            let nz = @boolFromInt(a - 2);
            let bits: @Vector(4, u8) = @intFromBool(nz);
            let ok = wide[0] == 257 and narrow[0] == 1 and f[2] == -3.0 and back[3] == 4 and
                bits[1] == 0 and bits[0] == 1;
            return if (ok) 42; else 0;
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime {
                const v: @Vector(2, i32) = .{ 1, 300 };
                const w: @Vector(2, i64) = @intCast(v);
                const f: @Vector(2, f32) = @floatFromInt(v);
                @assert(w[1] == 300 and f[0] == 1.0);
                @assert(@reduce(.add, @min(v, 100)) == 101);
                @assert(@mulAdd(@Vector(2, f32), .{ 1.0, 2.0 }, 2.0, 1.0)[1] == 5.0);
            }
            return 42;
        };
    )") == 42);
}

TEST_CASE("Vector math and casts reject lanes that don't fit", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut k: i32 = 300;
            k += 0;
            let v: @Vector(2, i32) = .{ 1, k };
            let n: @Vector(2, u8) = @intCast(v);
            return n[0];
        };
    )") != 1);
}

TEST_CASE("Vector math builtins check their lane types") {
    const auto check{[](std::string_view body, std::string_view needle) {
        const auto diags{helpers::type_check_diags(
            fmt::format("pub const f = fn(): void {{ let a: @Vector(4, i32) = .{{ 1, 2, 3, 4 }}; "
                        "let fl: @Vector(4, f32) = @splat(1.0); {} }};",
                        body))};
        CHECK(diags.message_contains(needle));
    }};
    check("let s = @sqrt(a);", "'@sqrt': expects float lanes");
    check("let m = @mod(fl, 2.0);", "'@mod': expects integer lanes");
    check("let m = @min(a, fl);", "expects a '@Vector(4, i32)'; found '@Vector(4, f32)'");
    check("let w: @Vector(2, i64) = @intCast(a);",
          "converts a vector to a vector of as many lanes");
    check("let w: @Vector(4, i32) = @floatFromInt(fl);",
          "can't convert 'f32' lanes to 'i32' lanes");
}

TEST_CASE("A generic instantiated with two vector types keeps one instance per type") {
    CHECK(helpers::compile_and_run(R"(
        const first = fn(comptime V: type, v: V): @typeInfo(V).vector.child { return v[0]; };
        pub const main = fn(): i32 {
            let a: @Vector(4, u8) = .{ 7, 0, 0, 0 };
            let b: @Vector(2, f32) = .{ 1.5, 0.0 };
            return if (first(@Vector(2, f32), b) == 1.5) @intCast(first(@Vector(4, u8), a)); else 0;
        };
    )") == 7);
}

TEST_CASE("Compile-time code writes the lanes of an undefined vector") {
    CHECK(helpers::compile_and_run(R"(
        const iota = fn(comptime V: type): V {
            let mut v: V = undefined;
            for (0..@typeInfo(V).vector.len) |i| { v[i] = @intCast(i); }
            return v;
        };
        pub const main = fn(): i32 {
            comptime {
                const v = iota(@Vector(4, u8));
                @assert(v[3] == 3);
            }
            return 42;
        };
    )") == 42);
}

TEST_CASE("A signature type built from an `auto` parameter's type waits for the instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Lanes = fn(comptime V: type): comptime_int { return @typeInfo(V).vector.len; };
        const bitmask = fn(v: auto): @Int(.{ .signedness = .unsigned, .bits = Lanes(@TypeOf(v)) }) {
            return @bitCast(v);
        };
        pub const main = fn(): i32 {
            let v: @Vector(4, i32) = .{ 5, -1, 7, 0 };
            let m = bitmask(v > 0);
            comptime { @assert(@TypeOf(m) == u4); }
            return @intCast(m);
        };
    )") == 0b0101);
}

} // namespace ghoti::tests
