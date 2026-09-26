#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@backingInt and @fromBackingInt round-trip an enum through its underlying type") {
    CHECK(helpers::compile_and_run(R"(
        const Level := enum : u8 { low = 3, mid = 7, high = 11 };

        pub const main := fn(): i32 {
            var l: Level = .mid;
            const raw: u8 = @backingInt(l);
            if (raw != 7u8) { return 1; }
            var n: u8 = 11;
            const h: Level = @fromBackingInt(n);
            if (h != .high) { return 2; }
            const explicit := @fromBackingInt(Level, 3u8);
            if (explicit != .low) { return 3; }
            return @as(i32, @backingInt(h));
        };
    )") == 11);
}

TEST_CASE("@fromBackingInt accepts any value of a non-exhaustive enum") {
    CHECK(helpers::compile_and_run(R"(
        const Flags := enum { none, one, two, _ };

        pub const main := fn(): i32 {
            var n: i32 = 40;
            const f := @fromBackingInt(Flags, n);
            return @backingInt(f) + 2;
        };
    )") == 42);
}

TEST_CASE("@backingInt folds for a compile-time enum value") {
    CHECK(helpers::compile_and_run(R"(
        const Code := enum { ok = 5, fatal = 9 };
        constexpr c: i32 = @backingInt(Code.fatal);
        constexpr k: Code = @fromBackingInt(5);

        pub const main := fn(): i32 {
            if (k != .ok) { return 1; }
            return c;
        };
    )") == 9);
}

TEST_CASE("@backingInt and @fromBackingInt round-trip a packed struct") {
    CHECK(helpers::compile_and_run(R"(
        const Flags := packed struct { a: bool, b: u3, c: u4, };

        pub const main := fn(): i32 {
            var f: Flags = .{ .a = true, .b = 5, .c = 9 };
            const raw: u8 = @backingInt(f);
            if (@as(i32, raw) != 1 + (5 << 1) + (9 << 4)) { return 1; }
            var bits: u8 = 0b1010_0110;
            const g: Flags = @fromBackingInt(bits);
            if (g.a) { return 2; }
            if (@as(i32, g.b) != 3) { return 3; }
            if (@as(i32, g.c) != 10) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@backingInt and @fromBackingInt round-trip a packed union") {
    CHECK(helpers::compile_and_run(R"(
        const Word := packed union { whole: u16, low: u8, };

        pub const main := fn(): i32 {
            var n: u16 = 0x1234;
            const w: Word = @fromBackingInt(n);
            if (@as(i32, w.low) != 0x34) { return 1; }
            const back: u16 = @backingInt(w);
            if (back != 0x1234u16) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@backingInt of a tagged union is its active tag") {
    CHECK(helpers::compile_and_run(R"(
        const Shape := union { circle: f32, square: i32, none: void, };

        const tag_of := fn(s: Shape): i32 { return @backingInt(s); };

        pub const main := fn(): i32 {
            var s: Shape = .{ .square = 4 };
            if (tag_of(s) != 1) { return 1; }
            s = .{ .none = {} };
            if (@backingInt(s) != 2) { return 2; }
            return tag_of(.{ .circle = 1.5 });
        };
    )") == 0);
}

} // namespace ghoti::tests
