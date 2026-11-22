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

// Both pipelines' outcome for a one-file program
[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

} // namespace

TEST_CASE("Function attributes reach the GIR function", "[uir]") {
    const auto diff{diff_source(R"(
        @[inline(.never), align(32)]
        const slow = fn(x: i32): i32 { return x + 1; };
        @[inline(.always)]
        const fast = fn(x: i32): i32 { return x * 2; };
        @[no_builtins]
        const plain = fn(): void {};
        pub const main = fn(): i32 { plain(); return slow(1) + fast(2); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A global's alignment and an over-aligned field match the old layout", "[uir]") {
    const auto diff{diff_source(R"(
        const S = struct { a: u8, @[align(16)] b: u8 };
        @[align(64)]
        pub let mut counter: u32 = 1;
        pub const main = fn(): i32 {
            let s: S = .{ .a = 1, .b = 2 };
            counter += s.b;
            return @sizeOf(S);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A misplaced attribute is reported as the old pipeline reports it", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        @[naked, inline(.always)]
        const f = fn(): void {};
        pub const g = fn(): void { f(); };
    )")};
    CHECK(run.codes() == std::vector{error::ILLEGAL_ATTRIBUTE});
    CHECK(run.messages().front() == "A 'naked' function cannot be 'inline(.always)'");
}

TEST_CASE("A module-level comptime block runs", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const N = 3;
        comptime { @assert(N == 4, "N must be 4"); }
    )")};
    CHECK(run.codes() == std::vector{error::STATIC_ASSERTION_FAILED});
    CHECK(run.messages().front() == "@assert failed at compile time: N must be 4");
}

TEST_CASE("A compile-time slice reads like the array it views", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const items: [3]u8 = .{ 4, 5, 6 };
        const view: []u8 = items;
        const len = view.len;
        const second = view[1];
        const letter = "abc"[2];
    )")};
    CHECK(run.int_value("len") == 3);
    CHECK(run.int_value("second") == 5);
    CHECK(run.int_value("letter") == 'c');
}

TEST_CASE("A compile-time slice used at runtime is backed by a stack array", "[uir]") {
    const auto diff{diff_source(R"(
        const items: [3]u8 = .{ 4, 5, 6 };
        const view: []u8 = items;
        pub const main = fn(): i32 { let s: []u8 = view; return s[2]; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::LEGACY_BUG);
}

TEST_CASE("@TypeOf names its operand's type without running it", "[uir]") {
    const auto diff{diff_source(R"(
        let mut calls: i32 = 0;
        const bump = fn(): i64 { calls += 1; return 2; };
        pub const main = fn(): i32 {
            let x: @TypeOf(bump()) = 5;
            const T = @TypeOf(x, 1);
            let y: T = 3;
            let sum: i32 = @intCast(x + y);
            return sum + calls;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("@TypeOf of operands with no peer type is reported", "[uir]") {
    const auto run{helpers::uir_rejected("const T = @TypeOf(@as(i32, 1), true);")};
    CHECK(run.codes() == std::vector{error::NO_PEER_TYPE});
    CHECK(run.messages().front().starts_with("'@TypeOf': "));
}

TEST_CASE("@typeName, @tagName and @bitSizeOf fold at compile time", "[uir]") {
    const auto diff{diff_source(R"(
        const Color = enum { red, green };
        const Shape = union(enum) { dot: void, line: u32 };
        pub const main = fn(): i32 {
            const name = @typeName(Color);
            const tag = @tagName(Color.green);
            const active = @tagName(Shape{ .line = 4 });
            const total = name.len + tag.len + active.len + @bitSizeOf(u24);
            return @intCast(total);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("@typeInfo describes aggregates, enums and scalars", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const S = struct { pub a: u8, b: i32 = 7 };
        const E = enum : u8 { x = 2, y };
        const U = union(enum) { p: u8, q: bool };
        const fields = @typeInfo(S).@"struct".fields.len;
        const second_default_is_set = @typeInfo(S).@"struct".fields[1].default_value != nullptr;
        const y_value = @typeInfo(E).@"enum".fields[1].value;
        const u_tagged = @typeInfo(U).@"union".tagged;
        const bits = @typeInfo(i24).int.bits;
        const is_unsigned = @typeInfo(u8).int.signedness == .unsigned;
        const child_len = @typeInfo([3]u16).array.len;
    )")};
    CHECK(run.int_value("fields") == 2);
    CHECK(run.int_value("y_value") == 3);
    CHECK(run.int_value("bits") == 24);
    CHECK(run.int_value("child_len") == 3);
    CHECK(run.gir().contains("const second_default_is_set: bool = true"));
    CHECK(run.gir().contains("const u_tagged: bool = true"));
    CHECK(run.gir().contains("const is_unsigned: bool = true"));
}

TEST_CASE("@typeInfo of a function reflects its declaration's attributes", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        @[inline(.never), align(16)]
        const f = fn(x: i32): bool { return x > 0; };
        const never = @typeInfo(f).function.inline_mode == .never;
        const aligned = @typeInfo(f).function.alignment;
        const params = @typeInfo(f).function.params.len;
    )")};
    CHECK(run.gir().contains("const never: bool = true"));
    CHECK(run.int_value("aligned") == 16);
    CHECK(run.int_value("params") == 1);
}

TEST_CASE("A pack parameter is a parameter per value, unrolled over", "[uir]") {
    const auto diff{diff_source(R"(
        const sum = fn(first: i32, rest...): i32 {
            let mut total: i32 = first;
            unroll for (rest) |x| { total += x; }
            return total + @intCast(rest.len);
        };
        const forward = fn(values...): i32 { return sum(values...); };
        const second = fn(values...): i32 { return values[1]; };
        pub const main = fn(): i32 { return sum(1, 2, 3) + sum(4) + forward(5, 6) + second(7, 8); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An `unroll for` repeats its body per element at runtime", "[uir]") {
    const auto diff{diff_source(R"(
        const table: [3]i32 = .{ 5, 6, 7 };
        pub const main = fn(): i32 {
            let mut total: i32 = 0;
            unroll for (table, 0..) |v, i| { total += v * @intCast(i); }
            unroll for (0..2) |k| { if (k == 1) { total += 1; } }
            return total;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A comptime_int built from reflected bits folds exactly", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const max_unsigned = fn(comptime T: type): comptime_int {
            const info = @typeInfo(T).int;
            return (1 << info.bits) - 1;
        };
        const max64 = max_unsigned(u64) == 18446744073709551615;
    )")};
    CHECK(run.gir().contains("const max64: bool = true"));
}

TEST_CASE("A runtime `@assert` and `@verify` call their handlers", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(x: i32): i32 {
            @assert(x > 0, "x must be positive");
            @verify(x < 10);
            @assert(true);
            return x;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@Int` and `@Float` build the types their descriptors name", "[uir]") {
    const auto diff{diff_source(R"(
        const U12 = @Int(.{ .bits = 12, .signedness = .unsigned });
        const I7 = @Int(.{ .bits = 7, .signedness = .signed });
        const F = @Float(.{ .bits = 64 });
        pub const widen = fn(x: U12, y: I7, z: F): F {
            return z + @floatFromInt(x) + @floatFromInt(y);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@Int` with a width it has no type for is reported", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const T = @Int(.{ .bits = 0, .signedness = .signed });
        pub const f = fn(x: T): T { return x; };
    )")};
    CHECK(run.codes() == std::vector{error::LITERAL_OUT_OF_RANGE});
    CHECK(run.messages().front() == "'@Int' has no signed 0-bit integer type; widths are 1..65535");
}

TEST_CASE("The target builtins name the prelude's enums", "[uir]") {
    const auto diff{diff_source(R"(
        pub const on_64 = fn(): bool {
            return @targetArch() == .x86_64 or @targetArch() == .aarch64;
        };
        pub const is_windows = fn(): bool { return @targetOs() == .windows; };
        pub const family = fn(): builtin.Family { return @targetFamily(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Math builtins fold known operands and call the builtin otherwise", "[uir]") {
    const auto diff{diff_source(R"(
        pub const counts = fn(x: u32): u32 { return @clz(x) + @ctz(x) + @popCount(x); };
        pub const extremes = fn(a: i32, b: i32): i32 { return @max(a, b) - @min(a, 3) + @abs(a); };
        pub const divide = fn(a: i64, b: i64): i64 {
            return @divTrunc(a, b) + @divFloor(a, b) + @rem(a, b) + @mod(a, b);
        };
        pub const roots = fn(x: f64): f64 { return @sqrt(x) + @floor(x) + @ceil(x) + @log2(x); };
        pub const known = fn(): i32 { return @max(2, 5) + @abs(-3); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`++` joins compile-time strings and `@panic` calls the handler", "[uir]") {
    const auto diff{diff_source(R"(
        const greet = fn(comptime who: []u8): []u8 { return "hello " ++ who ++ "!"; };
        const nums = [2]i32{ 1, 2 } ++ [1]i32{ 3 };
        pub const name = fn(): []u8 { return greet("there"); };
        pub const third = fn(): i32 { return nums[2]; };
        pub const fail = fn(x: i32): i32 {
            if (x < 0) { @panic("negative"); }
            return x;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`unroll while` copies its body per iteration under compile-time control", "[uir]") {
    const auto diff{diff_source(R"(
        pub const count = fn(x: i32): i32 {
            comptime let mut i: usize = 0;
            let mut total: i32 = x;
            unroll while (i < 4) {
                i += 1;
                if comptime (i == 2) { continue; }
                total += @intCast(i);
            }
            return total;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

} // namespace ghoti::tests
