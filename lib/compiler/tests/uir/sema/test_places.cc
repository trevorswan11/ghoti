#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/utility.hh>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

auto check_match(std::string_view text) -> void {
    const auto diff{diff_source(text)};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

} // namespace

TEST_CASE("`@embed` is a file's bytes as a constant array", "[uir]") {
    tempfile embedded{"ghoti_uir_embed"};
    {
        std::ofstream out{embedded.path};
        out << "UirEmbed";
    }

    const auto path{embedded.path.generic_string()};
    check_match(fmt::format(R"(
        pub const main = fn(): i32 {{
            let a = @embed("{0}");
            let b = @embed("{0}");
            return @intCast(i32, a.len + b.len);
        }};
    )",
                            path));

    const auto missing{helpers::uir_rejected(R"(const data = @embed("/no/such/file.bin");)")};
    CHECK(missing.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK(missing.messages().front().starts_with("failed to read embedded file"));
}

TEST_CASE("`@dynCast` reads a `dyn` object's data pointer", "[uir]") {
    check_match(R"(
        const N = interface { pub const get = fn(&self): i32; };
        const Cell = struct { n: i32 };
        impl N for Cell { pub const get = fn(&self): i32 { return self.n; }; }
        const bump = fn(w: &mut dyn N): i32 {
            let mut c: &mut Cell = @dynCast(&mut Cell, w);
            c.n = c.n + 1;
            let p: ^Cell = @dynCast(^Cell, w);
            return p.n;
        };
        pub const main = fn(): i32 {
            let mut cell = Cell{ .n = 41 };
            return bump(&mut cell);
        };
    )");

    const auto run{helpers::uir_rejected(R"(
        const f = fn(x: &i32): i32 { let p: ^i32 = @dynCast(^i32, x); return *p; };
    )")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
}

TEST_CASE("`@typeInfo` of a generic function reflects the attributes no parameter decides",
          "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        @[inline(.always), discardable, align(16)]
        const g = fn(comptime T: type, x: T): T { return x; };
        const pick = fn(comptime N: usize): usize {
            @branchHint(.cold);
            return N;
        };
        @[inline(if (N > 4) .never else .always)]
        const f = fn(comptime N: usize): usize {
            comptime { @assert(N > 4 or @typeInfo(f).function.inline_mode == .always); }
            return N;
        };
        comptime {
            @assert(@typeInfo(g).function.inline_mode == .always);
            @assert(@typeInfo(g).function.discardable);
            @assert(@typeInfo(g).function.alignment == 16);
            @assert(@typeInfo(pick).function.cold);
        }
        pub const main = fn(): i32 {
            let a = pick(2);
            let b = f(3);
            return g(i32, 1);
        };
    )"));

    const auto run{helpers::uir_rejected(R"(
        @[inline(if (N > 4) .never else .always)]
        const f = fn(comptime N: usize): usize { return N; };
        comptime { @assert(@typeInfo(f).function.inline_mode == .always); }
    )")};
    CHECK(run.codes() == std::vector{error::ILLEGAL_ATTRIBUTE});
}

TEST_CASE("A global may hold the address of a value with no storage of its own", "[uir]") {
    check_match(R"(
        const S = struct { a: i32 };
        const N = &22;
        const R = &@as(i32, 5);
        const P = ^@as(i32, 7);
        const T = &S{ .a = 9 };
        const val: i32 = 10;
        const r_const: &i32 = &val;
        const read = fn(r: &i32): i32 { return r; };
        const ptr = fn(): ^i32 { return P; };
        pub const main = fn(): i32 {
            let same = ptr() == ptr();
            return 20 + N + read(R) + *ptr() + T.a + @as(i32, @intFromBool(same)) + r_const;
        };
    )");
}

TEST_CASE("A `dyn` call to a discardable interface method may be discarded", "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        const Closer = interface { @[discardable] pub const close = fn(&self): i32; };
        const File = struct { fd: i32 };
        impl Closer for File { pub const close = fn(&self): i32 { return self.fd; }; }
        pub const main = fn(): i32 {
            let mut f: File = .{ .fd = 1 };
            let c: &dyn Closer = &f;
            c.close();
            return 0;
        };
    )"));
}

TEST_CASE("`auto` patterns take nested arrays and slices of arrays", "[uir]") {
    check_match(R"(
        const sum3 = fn(xs: [3]auto): i32 { return @intCast(i32, xs[0] + xs[1] + xs[2]); };
        const total = fn(xs: &mut [3]mut auto): i32 {
            xs[0] += 1;
            return @intCast(i32, xs[0] + xs[1] + xs[2]);
        };
        const sum = fn(xs: []auto): i64 {
            let mut t: i64 = 0;
            for (xs) |x| { t += x; }
            return t;
        };
        pub const main = fn(): i64 {
            let mut b: [3]mut u8 = .{ 1, 2, 3 };
            let a = [_]i32{ 1, 2, 3 };
            let t = total(&mut b) + sum3(b);
            return @as(i64, t) + sum(a) + sum(&a);
        };
    )");
}

TEST_CASE("An untyped array literal meeting a scalar takes the vector it is used as", "[uir]") {
    check_match(R"(
        const V = @Vector(4, i32);
        pub const main = fn(): i32 {
            let a: V = .{ -3, 8, 15, -1 } * 2 - 1;
            return a[0] + a[3];
        };
    )");
}

TEST_CASE("Runtime arrays meeting as a slice are sliced where they live", "[uir]") {
    check_match(R"(
        let mut flag: bool = true;
        let mut three: [3]i32 = .{ 1, 2, 3 };
        let mut five: [5]mut i32 = .{ 10, 20, 30, 40, 50 };
        const mk3 = fn(): [3]i32 { return .{ 1, 2, 3 }; };
        const mk5 = fn(): [5]i32 { return .{ 10, 20, 30, 40, 50 }; };
        pub const main = fn(): i32 {
            let a = if (flag) three else five;
            let b = if (flag) mk3() else mk5();
            return @intCast(a.len + b.len);
        };
    )");
}

TEST_CASE("A pointer is truthy in `and`, `or` and `!`", "[uir]") {
    check_match(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 5;
            let mut p: ^i32 = ^x;
            let n: ^i32 = nullptr;
            let with_and: i32 = if (p and n == nullptr) 1 else 0;
            let with_or: i32 = if (n or p) 1 else 0;
            let none: i32 = if (!n) 100 else 0;
            return with_and * 10 + with_or + none;
        };
    )");

    const auto run{helpers::uir_rejected("const f = fn(): bool { return !1; };")};
    CHECK(run.codes() == std::vector{error::OPERATOR_TYPE_MISMATCH});
}

TEST_CASE("An empty array is built at runtime", "[uir]") {
    check_match(R"(
        pub const main = fn(): void {
            let a = [_]i32{};
            for (a) |_| {}
        };
    )");
}

TEST_CASE("An `unroll for` leaves or repeats only through compile-time control flow", "[uir]") {
    check_match(R"(
        pub const main = fn(): i32 {
            let mut sum: i32 = 0;
            unroll for (0..5) |v| {
                if comptime (v == 2) { continue; }
                if comptime (v == 4) { break; }
                sum = sum + v;
            }
            return sum;
        };
    )");

    const auto run{helpers::uir_rejected(R"(
        pub const main = fn(): i32 {
            let mut sum: i32 = 0;
            let mut stop: i32 = 3;
            unroll for (0..5) |v| {
                if (v == stop) { break; }
                sum = sum + v;
            }
            return sum;
        };
    )")};
    CHECK(run.codes() == std::vector{error::UNROLLED_LOOP_BREAK});
}

TEST_CASE("A returned reference is read before a branching right side", "[uir]") {
    check_match(R"(
        let mut G: i32 = 3;
        const P = struct { a: i32, pub const get = fn(&self): &i32 { return &self.a; }; };
        const g = fn(): ^mut i32 { return ^mut G; };
        pub const main = fn(): i32 { let p: P = .{ .a = 2 }; return p.get() + *g(); };
    )");
}

TEST_CASE("An `auto` field takes its default's type", "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        const S = struct { a: auto = 42 };
        comptime { @assert(@TypeOf(S{}.a) == i32); }
    )"));

    const auto no_default{helpers::uir_rejected("const S = struct { a: auto }; const s: S = .{};")};
    CHECK(no_default.codes() == std::vector{error::ILLEGAL_AUTO_USAGE});
    const auto in_union{
        helpers::uir_rejected("const U = union(enum) { a: auto }; const u: U = .{ .a = 1 };")};
    CHECK(in_union.codes() == std::vector{error::ILLEGAL_AUTO_USAGE});
}

TEST_CASE("`*slice` takes an open end's length and looks through a `let`", "[uir]") {
    check_match(R"(
        pub const main = fn(): i32 {
            let mut arr: [5]mut u8 = .{ 1, 2, 3, 4, 5 };
            let mut m: [3]mut u8 = *arr[2..];
            m[0] = 10;
            let mut i: usize = 1;
            i += 1;
            let s = arr[i..][0..3];
            let c = *s;
            return m[0] + m[2] + c[1];
        };
    )");
}

TEST_CASE("`@field` with a literal name is assignable", "[uir]") {
    check_match(R"(
        const Point = struct {
            x: i32,
            let mut counter: i32 = 100;
        };
        pub const main = fn(): i32 {
            let mut p = Point{ .x = 1 };
            @field(p, "x") = 10;
            @field(Point, "counter") = 50;
            return @field(Point, "counter") + Point.counter + p.x;
        };
    )");
}

TEST_CASE("A known value may narrow through `@as`, a runtime one may not", "[uir]") {
    check_match(R"(
        const POS: i32 = 42;
        pub const main = fn(): i32 {
            const u: u32 = POS;
            return @as(i32, u);
        };
    )");

    const auto run{
        helpers::uir_rejected("pub const f = fn(x: usize): i64 { return @as(i64, x); };")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(run.messages().front() == "`@as` cannot narrow 'usize' to 'i64'; use `@intCast`");
}

TEST_CASE("A labeled block a known condition decides may break with a type", "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        pub const main = fn(): i32 {
            return @intCast(i32, @sizeOf(blk: {
                if (1 + 1 == 2) { break :blk i32; } else { break :blk f64; }
            }));
        };
    )"));
}

TEST_CASE("A local `const` slice of a runtime array is rejected", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        pub const main = fn(): i32 {
            let mut buf = [3]u8{ 1, 2, 3 };
            const s = buf[0..2];
            return @intCast(s.len);
        };
    )")};
    CHECK(run.codes() == std::vector{error::COMPTIME_MUT_NOT_FOLDABLE});
}

} // namespace ghoti::tests
