#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("A reference to a closure or generic function becomes an erased `fn`") {
    const std::string_view param{GENERATE("fn(n: i32): i32", "&dyn Fn(n: i32): i32")};
    CHECK(helpers::compile_and_run(fmt::format(R"(
            const apply = fn(g: {}, v: i32): i32 {{ return g(v); }};
            pub const main = fn(): i32 {{
                let off: i32 = 3;
                let c = fn(x: i32): i32 {{ return x + off; }};
                let f = fn(x: auto): i32 {{ return @as(i32, @intCast(x)) * off; }};
                return apply(&c, 1) + apply(&f, 2);
            }};
        )",
                                               param)) == 4 + 6);
}

TEST_CASE("A closure captures a capturing generic function and calls it at several types") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let off: i32 = 2;
            let f = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            let g = move fn(): i32 { return f(@as(u8, 3)) + f(@as(i64, 4)); };
            let h = fn(): i32 { return f(@as(u8, 5)); };
            return g() + h();
        };
    )") == 5 + 6 + 7);
    CHECK(helpers::compile_and_run(R"(
        const make = fn(off: i32): auto {
            let f = move fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            return move fn(): i32 { return f(@as(u8, 1)) + f(@as(i64, 2)); };
        };
        pub const main = fn(): i32 { let g = make(10); return g(); };
    )") == 11 + 12);
}

TEST_CASE("A module's or a type's generic function is a value through its qualified name") {
    constexpr std::string_view lib{R"(
        pub const twice = fn(x: auto): @TypeOf(x) { return x * 2; };
        pub const apply = fn(g: auto, v: i32): i32 { return g(v); };
    )"};
    CHECK(helpers::compile_and_run(R"(
            import "lib.gh" as m;
            const S = struct {
                const thrice = fn(x: auto): @TypeOf(x) { return x * 3; };
            };
            const apply_erased = fn(g: fn(n: i32): i32, v: i32): i32 { return g(v); };
            pub const main = fn(): i32 {
                let off: i32 = 1;
                let f = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
                return m.apply(m.twice, 2) + m.apply(S.thrice, 3) + apply_erased(m.twice, 4) +
                       apply_erased(S.thrice, 5) + m.apply(f, 6);
            };
        )",
                                   {mock_file{"lib.gh", lib, "m"}}) == 4 + 9 + 8 + 15 + 7);
}

TEST_CASE("Capturing generic functions work across modules") {
    constexpr std::string_view lib{R"(
        pub const run = fn(off: i32): i32 {
            let mut hits: i32 = 0;
            let f = fn(x: auto): i32 { hits += 1; return @as(i32, @intCast(x)) + off; };
            return f(@as(u8, 1)) + f(@as(i64, 2)) + hits;
        };
        pub const make = fn(off: i32): auto {
            return move fn(x: auto): i32 { return @as(i32, @intCast(x)) * off; };
        };
        pub const apply = fn(g: auto, v: i32): i32 { return g(v); };
    )"};
    CHECK(helpers::compile_and_run(R"(
            import "lib.gh" as lib;
            pub const main = fn(): i32 {
                let f = lib.make(3);
                let k: i32 = 2;
                let g = fn(x: auto): i32 { return @as(i32, @intCast(x)) - k; };
                return lib.run(10) + f(@as(u8, 2)) + f(@as(i64, 3)) + lib.apply(f, 1) +
                       lib.apply(g, 5);
            };
        )",
                                   {mock_file{"lib.gh", lib, "lib"}}) == 25 + 6 + 9 + 3 + 3);
}

TEST_CASE("A type constructor's generic member is a value of the instance it was taken from") {
    CHECK(helpers::compile_and_run(R"(
        const Box = fn(comptime T: type): type {
            return struct {
                v: T,
                pub const scale = fn(x: auto): T { return @as(T, @intCast(x)) * 2; };
            };
        };
        const apply = fn(g: auto, v: i32): i32 { return @intCast(g(v)); };
        pub const main = fn(): i32 {
            let g = Box(i64).scale;
            return apply(Box(i64).scale, 5) + apply(Box(i32).scale, 1) + @as(i32, @intCast(g(@as(u8, 3))));
        };
    )") == 10 + 2 + 6);
}

TEST_CASE("A generic method is called through its type or a value of it with the receiver first") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct {
            v: i32,
            const add = fn(&self, x: auto): i32 { return self.v + @as(i32, @intCast(x)); };
        };
        const apply = fn(g: auto, s: &S, v: i32): i32 { return g(s, v); };
        const apply_erased = fn(g: fn(s: &S, x: i32): i32, s: &S, v: i32): i32 { return g(s, v); };
        pub const main = fn(): i32 {
            let s: S = .{ .v = 10 };
            let m = S.add;
            return S.add(&s, @as(u8, 1)) + m(&s, @as(i64, 2)) + s.add(@as(u8, 3)) +
                   apply(S.add, &s, 4) + apply_erased(S.add, &s, 5);
        };
    )") == 11 + 12 + 13 + 14 + 15);
}

TEST_CASE("A generic closure body runs its `defer`s on each return") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let base: i32 = 5;
            let mut log: i32 = 0;
            let pick = fn(x: auto): i32 {
                defer { log += 1; }
                if (x > 0) { return @as(i32, @intCast(x)) + base; }
                return 0;
            };
            return pick(@as(i64, 1)) + pick(@as(i8, -1)) + log;
        };
    )") == 6 + 0 + 2);
}

TEST_CASE("Closures and generic local functions run in a `comptime` block") {
    CHECK(helpers::compile_and_run(R"(
        const run = fn(): i32 {
            comptime {
                const k: i32 = 4;
                let mut n: i32 = 0;
                let add = fn(x: i32): void { n += x * k; };
                let scale = fn(x: auto): i32 { return @as(i32, @intCast(x)) * k; };
                const plain = fn(x: i32): i32 { return x + 1; };
                add(2);
                add(1);
                @assert(n == 12);
                @assert(scale(@as(u8, 2)) == 8 and scale(@as(i64, 3)) == 12);
                @assert(plain(1) == 2);
                let mut c: i32 = 0;
                let bump = move fn(x: auto): i32 { c += @intCast(x); return c; };
                _ = bump(@as(u8, 1));
                @assert(bump(@as(i64, 2)) == 3 and c == 0);
            }
            return 1;
        };
        pub const main = fn(): i32 { return run(); };
    )") == 1);
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            comptime {
                let mut n: i32 = 0;
                let add = fn(x: i32): void { n += x; };
                add(2);
                @assert(n == 3);
            }
            return 0;
        };
    )");
}

TEST_CASE("A closure inside a function folded at compile time keeps its writes") {
    CHECK(helpers::compile_and_run(R"(
        const sum_with = fn(n: i32, bias: i32): i32 {
            let mut s: i32 = 0;
            let add = fn(x: i32): void { s += x + bias; };
            for (0..n) |i| { add(@intCast(i)); }
            return s;
        };
        const apply = fn(g: auto, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };
        const total = sum_with(3, 1);
        const doubled = apply(twice, 21);
        pub const main = fn(): i32 {
            comptime { @assert(total == 6); }
            return total + doubled;
        };
    )") == 6 + 42);
}

} // namespace ghoti::tests
