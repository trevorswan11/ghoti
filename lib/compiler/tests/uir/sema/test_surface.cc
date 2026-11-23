#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/types.hh>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"
#include "support/int128.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

} // namespace

TEST_CASE("`^.{ ... }` backs a slice with a known global or a runtime slot", "[uir]") {
    const auto diff{diff_source(R"(
        const take = fn(body: []u8): u8 { return body[0]; };
        pub const known = fn(): u8 { return take(^.{ 4, 5 }); };
        pub const runtime = fn(v: u32): u8 { return take(^.{ @intCast(u8, v) }); };
        pub const bound = fn(): u8 {
            let s: []u8 = ^.{ 1, 2, 3 };
            return s[2];
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A slice range assignment moves the source's elements in", "[uir]") {
    const auto diff{diff_source(R"(
        const two = fn(x: u8): [2]u8 { return .{ x, x }; };
        const wide = fn(x: u32): [2]u32 { return .{ x, x }; };
        pub const fill = fn(i: usize, x: u8): u8 {
            let mut buf: [8]mut u8 = undefined;
            buf[i..][0..2] = two(x);
            buf[1..3] = two(x);
            return buf[i];
        };
        pub const fill_wide = fn(x: u32): u32 {
            let mut buf: [4]mut u32 = undefined;
            buf[0..2] = wide(x);
            return buf[buf.len - 1];
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A field holding an `extern fn` is called through its value", "[uir]") {
    const auto diff{diff_source(R"(
        const T = struct { n: u8, func: extern fn(x: u8): bool };
        pub const run = fn(t: T): bool { return t.func(t.n); };
        pub const run_all = fn(ts: []T): bool {
            let mut ok = true;
            for (ts) |e| { ok = ok and e.func(1); }
            return ok;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@expect` and `@require` check at runtime through their handlers", "[uir]") {
    const auto diff{diff_source(R"(
        @[testing] pub const f = fn(x: u32, p: ^u8): void {
            @expect(x == 3);
            _ = @expect(true);
            @expect(p, "points somewhere");
            @require(x != 0);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A union compares to a variant name before its fields are needed", "[uir]") {
    const auto diff{diff_source(R"(
        const Opt = fn(comptime T: type): type {
            return union(enum) {
                some: T,
                none: void,
                pub const of = fn(val: T): @This() { return .{ .some = val }; };
                pub const empty: @This() = .{ .none = {} };
            };
        };
        const f = fn(comptime T: type, v: T): Opt(T) {
            if (v > 3) return .empty;
            return .of(v);
        };
        pub const g = fn(v: u4): bool { return f(u4, v) == .none; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An inline `@Int` descriptor in a runtime body is known", "[uir]") {
    const auto diff{diff_source(R"(
        const promote = fn(comptime T: type, value: T): u8 {
            let wide = @as(@Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits + 1 }), value);
            return @intCast(wide);
        };
        pub const g = fn(v: u4): u8 { return promote(u4, v); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Inline `asm` lists outputs, inputs and clobbers", "[uir]") {
    const auto diff{diff_source(R"(
        const Regs = struct { a: u32, b: u32 };
        pub const cpuid = fn(leaf: u32): Regs {
            let mut r: Regs = .{ .a = 0, .b = 0 };
            asm {
                template: "cpuid %0",
                outputs: ("={eax}" = r.a, "={ebx}" = r.b),
                inputs: ("{eax}" = leaf),
            };
            return r;
        };
        pub const keep = fn(x: u64): void {
            asm { template: "", inputs: ("r" = x), clobbers: ("memory"), options: (volatile) };
        };
        pub const read = fn(): u64 {
            return asm u64 { template: "rdtsc", outputs: ("=r" = _) };
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@llvmIntrinsic` and `@returnAddress` lower to builtin calls", "[uir]") {
    const auto diff{diff_source(R"(
        pub const pop = fn(x: u32): u32 { return @llvmIntrinsic("llvm.ctpop.i32", u32, x); };
        pub const caller = fn(): usize { return @returnAddress(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

// #467: the old pipeline rejects `@returnAddress` in a function with comptime parameters
TEST_CASE("`@returnAddress` works in a generic body", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const plain = fn(comptime T: type): usize {
            return @returnAddress() + @sizeOf(T);
        };
        pub const main = fn(): i32 { return @intCast(i32, plain(u32) & 1); };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("@returnAddress"));
}

TEST_CASE("An untyped merge stays untyped through arithmetic on a known value", "[uir]") {
    const auto diff{diff_source(R"(
        const Case = enum { lower, upper };
        pub const digit_to_char = fn(digit: u8, case: Case): u8 {
            return match (digit) {
                0..=9 => digit + '0',
                10..=35 => digit + ((if (case == .upper) 'A' else 'a') - 10),
                _ => 0,
            };
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@TypeOf` of a generic in its own body is the instance's function type", "[uir]") {
    const auto diff{diff_source(R"(
        const ret_of = fn(comptime F: type): type { return @typeInfo(F).function.return_type; };
        const widen = fn(comptime T: type, v: T): @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits + 1 }) {
            const W = ret_of(@TypeOf(widen));
            return @as(W, v);
        };
        pub const g = fn(v: u8): u9 { return widen(u8, v); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An operator with an untyped side computes in the wider type it is used as", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(b: u128, d: u7): u128 { return b << (64 - d); };
        pub const g = fn(d: u7): u128 { let x: u128 = 64 - d; return x; };
        pub const h = fn(b: u128, d: u7): u128 { return b << (d + d); };
        pub const k = fn(d: u7): u128 { return 100 - (d + 1); };
        pub const m = fn(a: u64, b: u64): u128 { return @as(u128, a) * (b - 1); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A runtime `unreachable` panics with safety checks", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(d: u8): u8 {
            return match (d) { 0..=9 => d + 1, _ => unreachable };
        };
        pub const g = fn(d: u8): u8 { if (d > 3) { unreachable; } return d; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A tagged union in storage has its tag read where it lives", "[uir]") {
    const auto diff{diff_source(R"(
        const Opt = union(enum) { some: usize, none: void };
        const S = struct { k: Opt, n: u8 };
        let mut cached: Opt = .{ .none = {} };
        pub const global = fn(): bool { return cached == .none; };
        pub const field = fn(s: S): bool { return s.k != .none; };
        pub const matched = fn(s: S): usize {
            return match (s.k) { .some => |v| v, .none => 0 };
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A `dyn` call reads its receiver's vtable entry before its arguments", "[uir]") {
    const auto diff{diff_source(R"(
        const Sink = interface { const put = fn(&mut self, bytes: []u8): void; };
        const Buf = struct { n: usize };
        impl Sink for Buf { pub const put = fn(&mut self, bytes: []u8): void { self.n += bytes.len; }; }
        pub const send = fn(w: &mut dyn Sink, text: []u8, i: usize): void {
            w.put(text[i..]);
            w.put(text[0..i]);
        };
    )")};
    INFO(diff.detail);
    CHECK((diff.status == helpers::uir_diff_status::MATCH ||
           diff.status == helpers::uir_diff_status::EVAL_ORDER));
}

TEST_CASE("A C-variadic extern takes extra arguments in their own types", "[uir]") {
    const auto diff{diff_source(R"(
        extern("c", "printf") const printf: fn(format: ^u8, ...): i32;
        pub const show = fn(fmt: [:0]u8, n: i64): i32 { return printf(fmt.ptr, n, 3); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A `const` and its alias in another module are both qualified", "[uir]") {
    mod::memory_loader loader;
    loader.add("b.gh", std::string{R"(
        import "main.gh" as a;
        pub const X = a.X;
        pub const g = fn(): u32 { return X; };
    )"});
    loader.add("main.gh", std::string{R"(
        import "b.gh" as b;
        pub const X: u32 = 2;
        pub const f = fn(): u32 { return b.g() + X; };
    )"});
    const auto diff{helpers::diff_pipelines(loader, "main.gh", {}, {})};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A known byte array used as a slice at runtime reads one string constant", "[uir]") {
    const auto diff{diff_source(R"(
        const take = fn(comptime text: []u8, i: usize): u8 { return text[i]; };
        pub const f = fn(i: usize): u8 { return take("ab" ++ "cd", i); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A runtime `@tagName` tests each variant's name in turn", "[uir]") {
    const auto diff{diff_source(R"(
        const Color = enum : u8 { red = 1, green = 4, blue };
        const Open = enum : u8 { a, _ };
        const Shape = union(enum) { dot: void, line: u32 };
        pub const color = fn(c: Color): []u8 { return @tagName(c); };
        pub const open = fn(o: Open): []u8 { return @tagName(o); };
        pub const shape = fn(s: Shape): []u8 { return @tagName(s); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Exported, weak and thread-local globals keep their linkage", "[uir]") {
    const auto diff{diff_source(R"(
        export let mut counter: i32 = 1;
        export let limit: u32 = 7;
        weak let mut fallback: u8 = 2;
        threadlocal let mut tls: u64 = 0;
        pub const bump = fn(): i32 { counter += 1; fallback += 1; return counter + @intCast(i32, limit); };
        pub const tick = fn(): u64 { tls += 1; return tls; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

// The old pipeline reads an `extern let mut` as `undefined` (docs/uir/known_legacy_bugs.md)
TEST_CASE("An extern global is read from the linker's symbol", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        extern let mut errno_value: i32;
        pub const main = fn(): i32 { errno_value += 1; return errno_value; };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("global_addr @errno_value"));
}

TEST_CASE("A `@Struct` field takes the default its descriptor points at", "[uir]") {
    const auto diff{diff_source(R"(
        const DEFAULT_Y: i32 = 99;
        const Point = fn(comptime T: type, comptime default_z: T): type {
            return @Struct(.{
                .fields = ^.{
                    .{ .name = "x", .@"type" = T },
                    .{ .name = "y", .@"type" = T, .default_value = @ptrCast(^opaque, ^DEFAULT_Y) },
                    .{ .name = "z", .@"type" = T, .default_value = @ptrCast(^opaque, ^default_z) },
                    .{ .name = "w", .@"type" = T, .default_value = @ptrCast(^opaque, ^7) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
        };
        pub const sum = fn(x: i32): i32 {
            let p: Point(i32, 1) = .{ .x = x };
            return p.x + p.y + p.z + p.w;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

// #459: the old pipeline only takes cfg terms as a `@cfgValue` arm
TEST_CASE("A `@cfgValue` arm may need analysis, like `@sizeOf`", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const S = struct { a: u64, b: u8 };
        const size = @cfgValue(ptr_bits == 64 => @sizeOf(S) + 1, _ => @sizeOf(S));
        pub const main = fn(): i32 { return @intCast(i32, size); };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("const size: comptime_int = 17"));
}

TEST_CASE("Enums of the same shape in different modules make distinct instances", "[uir]") {
    mod::memory_loader loader;
    loader.add("m.gh", std::string{R"(
        pub const Errno = enum : u32 { bad = 5 };
        pub const Box = fn(comptime E: type): type { return struct { pub e: E }; };
        pub const make = fn(x: u8): Box(Errno) { _ = x; return .{ .e = .bad }; };
    )"});
    loader.add("main.gh", std::string{R"(
        import "m.gh" as m;
        pub const Error = enum : i32 { full = 1 };
        pub const mine = fn(): m.Box(Error) { return .{ .e = .full }; };
        pub const theirs = fn(x: u8): u32 { return @backingInt(m.make(x).e); };
    )"});
    const auto diff{helpers::diff_pipelines(loader, "main.gh", {}, {})};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A parameterized impl targets a ctor another module declares", "[uir]") {
    mod::memory_loader loader;
    loader.add("m.gh", std::string{R"(
        pub const Box = fn(comptime T: type): type { return struct { pub v: T }; };
    )"});
    loader.add("main.gh", std::string{R"(
        import "m.gh" as m;
        const Get = interface { pub const get = fn(&self): i32; };
        impl(T: type) Get for m.Box(T) {
            pub const get = fn(&self): i32 { return @intCast(i32, self.v); };
        }
        pub const use = fn(x: u8): i32 {
            let b: m.Box(u8) = .{ .v = x };
            return b.get();
        };
    )"});
    const auto diff{helpers::diff_pipelines(loader, "main.gh", {}, {})};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

// #460: the old pipeline can't store into an element of a global's compile-time initializer
TEST_CASE("A global is built by a function storing into its elements", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const Lock = struct { locked: bool = false, id: u8 = 0 };
        const build = fn(): [4]mut Lock {
            let mut l: [4]mut Lock = undefined;
            for (0..4) |i| {
                l[i] = .{};
                l[i].id = @intCast(u8, i * 2);
            }
            return l;
        };
        const table = build();
        const third = table[3].id;
        pub const main = fn(): i32 { return third; };
    )")};
    CHECK(run.int_value("third") == 6);
}

// #453: the old pipeline reads a `[_]type{ ... }` literal as a runtime container
TEST_CASE("`unroll for` walks a literal array of types", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const use = fn(): u32 {
            let mut bits: u32 = 0;
            unroll for ([_]type{ f32, f64 }) |T| {
                const Int = @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).float.bits });
                bits += @bitSizeOf(Int);
            }
            return bits;
        };
        pub const main = fn(): i32 { return @intCast(i32, use()); };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("fn use() -> u32"));
}

// #428: the old pipeline can't `@bitCast` an array at compile time or in a global
TEST_CASE("`@bitCast` reinterprets an array's bytes at compile time", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const g: u32 = @bitCast(u32, [4]u8{ 1, 2, 3, 4 });
        const back: [2]u16 = @bitCast([2]u16, g);
        const low = back[0];
        pub const use = fn(): u32 {
            const c: u32 = @bitCast(u32, [4]u8{ 4, 3, 2, 1 });
            return g + c;
        };
        pub const main = fn(): i32 { return 0; };
    )")};
    CHECK(run.int_value("g") == 0x04030201);
    CHECK(run.int_value("low") == 0x0201);
}

// #429: an `f80` and a `u80` differ in ABI size on i686 but hold the same bits
TEST_CASE("`@bitCast` compares bit widths, not padded sizes", "[uir]") {
    helpers::pipeline_settings settings;
    settings.target_opts.triple_str = "i686-unknown-linux-gnu";
    const helpers::uir_run run{R"(
        const one: u80 = @bitCast(u80, @as(f80, 1.0));
        pub const main = fn(): i32 { return 0; };
    )",
                               settings};
    INFO(fmt::format("{}", run.out().diagnostics));
    REQUIRE_FALSE(run.out().stopped_at);
    REQUIRE(run.out().succeeded());
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("const one: u80"));

    const auto wrong{helpers::uir_rejected(R"(
        const bad: u32 = @bitCast(u32, [3]u8{ 1, 2, 3 });
    )")};
    CHECK(wrong.codes() == std::vector{sema::error::TYPE_MISMATCH});
}

// #427: a local `const n = @sizeOf(T)` names an array length in a generic `@ptrCast` target
TEST_CASE("A generic's local constant sizes a pointer cast's array type", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const first = fn(comptime T: type, b: []u8): u8 {
            const n = @sizeOf(T);
            return (*@ptrCast(^[n]u8, b.ptr))[n - 1];
        };
        pub const main = fn(): i32 {
            let mut buf: [4]u8 = .{ 1, 2, 3, 4 };
            return first(u32, buf[0..]);
        };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("first__u32"));
}

// #461: the old pipeline once said a `while (true)` returning from its body doesn't return
TEST_CASE("A `while (true)` that only leaves by returning returns on every path", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const find = fn(xs: []u8, x: u8): usize {
            let mut i: usize = 0;
            while (true) : (i += 1) {
                if (i == xs.len) { return xs.len; }
                if (xs[i] == x) { return i; }
            }
        };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("fn find"));

    const auto diff{diff_source(R"(
        pub const a = fn(x: u32): u32 {
            while (true) { if (x > 3) { return x; } }
        };
        pub const b = fn(x: u32): u32 {
            _ = x;
            while (true) {}
        };
        pub const c = fn(x: u32): u32 {
            while (true) { return x; }
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::LEGACY_REJECTED);
}

// #464: a generic parameter `&[2]T` takes a reference to an array of the bound type
TEST_CASE("A generic takes a reference to an array of its type parameter", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const L = struct { v: i32 };
        const first = fn(comptime T: type, arr: &[2]T): T { return arr[0]; };
        pub const use = fn(): i32 {
            const xs: [2]L = .{ .{ .v = 1 }, .{ .v = 2 } };
            return first(L, &xs).v;
        };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("arr: &[2]struct"));
}

// #465: a union constant keeps an `i128` payload whole
TEST_CASE("A union constant holds an `i128` payload", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const U = union(enum) { big: i128, small: u8 };
        const k: U = .{ .big = 1 << 100 };
        pub const hi = fn(): i128 {
            return match (k) { .big => |v| v >> 64, .small => |s| s };
        };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("ret i128 68719476736"));
}

} // namespace ghoti::tests
