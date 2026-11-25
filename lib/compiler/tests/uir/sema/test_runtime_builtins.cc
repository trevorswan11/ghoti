#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

} // namespace

TEST_CASE("Atomic loads, stores, read-modify-writes, exchanges and fences", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(): i32 {
            let mut x: i32 = 0;
            let mut out: i32 = 0;
            @atomicStore(^mut x, 42, builtin.MemoryOrder.release);
            let old = @atomicRmw(i32, ^mut x, builtin.AtomicRmwOp.add, 5,
                                 builtin.MemoryOrder.acq_rel);
            let swapped = @cmpxchgStrong(i32, ^mut x, 47, 9, builtin.MemoryOrder.seq_cst,
                                         builtin.MemoryOrder.relaxed, ^mut out);
            @fence(builtin.MemoryOrder.seq_cst);
            let now = @atomicLoad(i32, ^mut x, builtin.MemoryOrder.acquire);
            return if (swapped) old + now; else out;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An atomic's memory orders are checked", "[uir]") {
    const auto load{helpers::uir_rejected(R"(
        pub const f = fn(): i32 {
            let mut x: i32 = 0;
            return @atomicLoad(i32, ^mut x, builtin.MemoryOrder.release);
        };
    )")};
    CHECK(load.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(load.messages().front() ==
          "'@atomicLoad' cannot use memory order 'release'; loads accept relaxed, acquire, or "
          "seq_cst");

    const auto stronger{helpers::uir_rejected(R"(
        pub const f = fn(): bool {
            let mut x: i32 = 0;
            let mut out: i32 = 0;
            return @cmpxchgWeak(i32, ^mut x, 0, 1, builtin.MemoryOrder.relaxed,
                                builtin.MemoryOrder.seq_cst, ^mut out);
        };
    )")};
    CHECK(stronger.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(stronger.messages().front() ==
          "'@cmpxchgWeak' failure memory order 'seq_cst' may not be stronger than its success "
          "order 'relaxed'");
}

TEST_CASE("Overflow builtins write the wrapped result and return whether it overflowed", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(a: u8, b: u8, s: u3): bool {
            let mut sum: u8 = 0;
            let mut shifted: u8 = 0;
            let over = @addWithOverflow(a, b, &mut sum);
            let lost = @shlWithOverflow(a, s, &mut shifted);
            return over or lost;
        };
        pub const lanes = fn(v: @Vector(4, i32)): @Vector(4, bool) {
            let mut out: @Vector(4, i32) = undefined;
            return @mulWithOverflow(v, 3, &mut out);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A `@branchHint(.cold)` body makes its function cold", "[uir]") {
    const auto diff{diff_source(R"(
        pub const rare = fn(x: i32): i32 {
            @branchHint(.cold);
            return if (x > 0) 1 else 2;
        };
        pub const pick = fn(x: i32): i32 {
            let mut y: i32 = 0;
            if (x > 0) {
                @branchHint(.likely);
                y = 1;
            } else {
                y = 2;
            }
            return y;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A `@branchHint` that doesn't lead its body is rejected", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        pub const f = fn(x: i32): i32 {
            let y = x + 1;
            @branchHint(.cold);
            return y;
        };
    )")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(run.messages().front() ==
          "@branchHint must be the first statement of an 'if'/'else' branch, a 'match' arm, or a "
          "function body");
}

TEST_CASE("`@setRuntimeSafety` scopes runtime checks to its body", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(a: i32, b: i32): i32 {
            let checked = a + b;
            {
                @setRuntimeSafety(false);
                return checked + a * b;
            }
        };
        pub const on = fn(): bool { return @runtimeSafety(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Target queries fold to the target's facts", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const bits = @targetPtrBits();
        const triple = @targetTriple();
        pub const little: i32 = if (@targetEndian() == .little) 1 else 0;
        pub const width: i32 = bits;
        pub const named: i32 = if (triple.len > 0) 1 else 0;
        pub const mode: i32 = if (@optimizeMode() == .debug) 1 else 0;
    )")};
    CHECK(run.int_value("little") == 1);
    CHECK(run.int_value("width") == 64);
    CHECK(run.int_value("named") == 1);
    CHECK(run.int_value("mode") == 1);
}

TEST_CASE("`@Pointer`, `@Slice`, `@Array`, `@Reference` and `@Fn` build types", "[uir]") {
    const auto diff{diff_source(R"(
        const P = @Pointer(.{ .child = i32, .is_mut = true, .is_volatile = false });
        const S = @Slice(.{ .child = u8, .sentinel = false, .is_mut = false, .is_volatile = false });
        const A = @Array(.{ .child = u16, .len = 3, .sentinel = false, .is_mut = false,
                            .is_volatile = false });
        const F = @Fn(builtin.FnInfo{ .params = [2]type{ i32, i32 }, .return_type = i32,
                                      .variadic = false, .has_self = false, .@"callconv" = .c });
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        pub const f = fn(p: P, s: S): i32 {
            const a: A = .{ 1, 2, 3 };
            const g: F = add;
            return *p + @intCast(i32, s.len) + a[2] + g(1, 2);
        };
        pub const literal = fn(): bool { return @TypeOf(.x) == @EnumLiteral(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Field queries read a struct's fields by name", "[uir]") {
    const auto diff{diff_source(R"(
        const S = struct { a: i32, b: u8 = 7 };
        pub const has = fn(): bool { return @hasField(S, "a") and !@hasField(S, "c"); };
        pub const kind = fn(): bool { return @fieldType(S, "b") == u8; };
        pub const fallback = fn(): u8 { return @fieldDefault(S, "b"); };
        pub const parent = fn(p: ^mut u8): ^mut S { return @fieldParentPtr(S, "b", p); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A pack's slice is a pack of the elements its bounds select", "[uir]") {
    const auto diff{diff_source(R"(
        const digits = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest) |v| { t = t * 10 + v; }
            return t;
        };
        const forward = fn(rest...): i32 { return digits(rest[1..]...); };
        pub const f = fn(): i32 { return forward(1, 2, 3); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A pack slice's bounds are checked against the pack", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const count = fn(rest...): i32 { return 0; };
        const forward = fn(rest...): i32 { return count(rest[1..5]...); };
        pub const f = fn(): i32 { return forward(1, 2, 3); };
    )")};
    CHECK(run.codes() == std::vector{error::PACK_INDEX_OUT_OF_RANGE});
    CHECK(run.messages().front() == "pack slice 1..5 out of range for 'rest' (len 3)");
}

TEST_CASE("Assigning a `let mut` reference writes its referent", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(): i32 {
            let mut w: i32 = 3;
            let mut r: &mut i32 = &mut w;
            r = 11;
            r += 1;
            return w;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A chain of `or`s starting with a name stores each default before its left side",
          "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(a: bool, b: bool, n: i32): bool {
            return a or !b or n != 0;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A `dyn` binds an interface's associated types by name", "[uir]") {
    const auto diff{diff_source(R"(
        const Src = interface { Item: type; pub const first = fn(&self): Item; };
        const Box = struct { v: i32 };
        impl Src for Box {
            const Item = i32;
            pub const first = fn(&self): Item { return self.v; };
        }
        const take = fn(s: &dyn Src(Item = i32)): i32 { return s.first(); };
        pub const f = fn(): i32 {
            let mut b = Box{ .v = 42 };
            return take(&b);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A range value takes the element type it is used as", "[uir]") {
    const auto diff{diff_source(R"(
        const sum = fn(xs: []u8): u32 { let mut s: u32 = 0; for (xs) |x| { s += x; } return s; };
        pub const f = fn(): u32 {
            let y: [4]u8 = 2..6;
            let d = 3..0;
            return sum(10..=12) + y[3] + @intCast(u32, d[0]);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A pointer is sliced with its bounds' order checked", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(p: ^mut u8, q: ^u8, n: usize): void {
            @memcpy(p[0..n], q[1..n]);
        };
        pub const g = fn(p: ^mut u8, n: usize): []mut u8 { return p[2..=n]; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An inferred cast meeting an untyped value takes the type it is used as", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(x: u32, pad: u3, s: bool): u8 {
            let low: u8 = @truncate(x) & 0x0f;
            let top: u8 = if (s) @bitCast(@bitCast(i8, @as(u8, low)) >> pad) else low >> pad;
            let both: u8 = if (s) @truncate(x) else @truncate(x >> 8);
            return low + top + both;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A by-value method reads a `let mut` receiver where it lives", "[uir]") {
    const auto diff{diff_source(R"(
        const S = struct {
            pub exp: u16,
            pub const get = fn(self): u16 { return self.exp; };
        };
        pub const f = fn(sign: u16): u16 {
            let mut dst: S = undefined;
            dst.exp = 0;
            dst.exp |= sign;
            return dst.get();
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A sentinel slice passes where a plain slice is wanted", "[uir]") {
    const auto diff{diff_source(R"(
        const E = enum { aa, bbb };
        const take = fn(s: []u8): usize { return s.len; };
        const pass = fn(v: auto): usize { return take(v); };
        pub const f = fn(e: E, z: [:0]u8): usize {
            return take(@tagName(e)) + take(z) + pass(z) + pass(@tagName(e));
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Closures in two modules are told apart in instance names", "[uir]") {
    mod::memory_loader loader;
    loader.add("a.gh", R"(
        const rt = fn(comptime T: type): type { return @typeInfo(T).function.return_type; };
        pub const fa = fn(): usize {
            let mut a: u8 = 2;
            let closure = fn(): f32 { a += 1; return 1.0; };
            _ = closure();
            return if (rt(@TypeOf(closure)) == f32) 10 else 0;
        };
    )");
    loader.add("main.gh", R"(
        import "a.gh" as a;
        const rt = fn(comptime T: type): type { return @typeInfo(T).function.return_type; };
        pub const f = fn(x: i32): usize {
            let offset: i64 = 40;
            let c = fn(val: i32): i64 { return @as(i64, val) + offset; };
            _ = c(x);
            return a.fa() + @as(usize, if (rt(@TypeOf(c)) == i64) 100 else 0);
        };
    )");
    const auto diff{helpers::diff_pipelines(loader, "main.gh", {}, {})};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A generic function's type is its open signature", "[uir]") {
    const auto diff{diff_source(R"(
        const rt = fn(comptime T: type): type { return T; };
        const id = fn(x: auto): @TypeOf(x) { return x; };
        pub const name = @typeName(@TypeOf(rt));
        pub const name2 = @typeName(@TypeOf(id));
        pub const ret_is_type = @typeInfo(@TypeOf(rt)).function.return_type == type;
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`void`, `undefined` and wide untyped values pass as runtime arguments", "[uir]") {
    const auto diff{diff_source(R"(
        const take = fn(val: auto): void { _ = val; };
        const count = fn(base: u32, args...): u32 { return base + @intCast(args.len); };
        const pick = fn(args...): u128 { return args[1]; };
        pub const f = fn(x: u32): u128 {
            take({});
            take(undefined);
            _ = count(x, 340282366920938463463374607431768211455, {});
            return pick(x, 340282366920938463463374607431768211455);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Known string arms meet as a sentinel slice", "[uir]") {
    const auto diff{diff_source(R"(
        const take = fn(s: []u8): usize { return s.len; };
        pub const f = fn(b: bool): usize { return take(if (b) "true" else "false"); };
        pub const g = fn(b: bool): usize { let s: []u8 = if (b) "yes" else "no"; return s.len; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A type declared in a runtime body is one type per set of captures", "[uir]") {
    const auto diff{diff_source(R"(
        const wrap = fn(comptime T: type, x: T): usize {
            const W = struct { v: T, n: u8 };
            let w: W = .{ .v = x, .n = 1 };
            return @sizeOf(W) + w.n;
        };
        pub const f = fn(): usize {
            const P = struct { a: i32, b: i64 };
            let p: P = .{ .a = 1, .b = 2 };
            return @sizeOf(P) + @intCast(usize, p.a) + wrap(u8, 3) + wrap(u64, 4);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Comparing an integer with an untyped number past its type folds", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const lt = fn(v: u32): bool { return v < 100000000000000000; };
        pub const ne = fn(v: i8): bool { return -300 != v; };
    )")};
    CHECK(run.gir().contains("ret bool true"));
    CHECK_FALSE(run.gir().contains("lt bool"));
}

} // namespace ghoti::tests
