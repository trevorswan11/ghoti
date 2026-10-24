#include <catch2/catch_test_macros.hpp>

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

    const auto unknown{helpers::type_check_diags(
        "pub const f = fn(n: usize): void { let a: @Vector(n, f32) = undefined; };")};
    CHECK(unknown.message_contains("'@Vector' needs a compile-time-known lane count"));
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

} // namespace ghoti::tests
