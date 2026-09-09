#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Top-level array const indexed with a compile-time-constant index") {
    CHECK(helpers::compile_and_run(R"(
        const ARR := [3]i32{7, 8, 9};
        pub const main := fn(): i32 {
            return ARR[1];
        };
    )") == 8);
}

TEST_CASE("Top-level array const indexed with a runtime (non-constant) index") {
    CHECK(helpers::compile_and_run(R"(
        const ARR := [3]i32{7, 8, 9};
        pub const main := fn(): i32 {
            var i: usize = 2;
            return ARR[i];
        };
    )") == 9);
}

TEST_CASE("Top-level struct const field access") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const P := Point{ .x = 7, .y = 8 };
        pub const main := fn(): i32 {
            return P.y;
        };
    )") == 8);
}

TEST_CASE("Top-level tagged union const active field access") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        const V := U{ .val = 42 };
        pub const main := fn(): i32 {
            return V.val;
        };
    )") == 42);
}

TEST_CASE("Top-level const initialized by a match with a capturing arm") {
    CHECK(helpers::compile_and_run(R"(
        const Payload := union { A: i32, B: i32 };
        const u := Payload{ .A = 5 };
        const y := match (u) {
            .A => |v| v,
            .B => 0,
        };
        pub const main := fn(): i32 {
            return y;
        };
    )") == 5);
}

TEST_CASE("Top-level struct const with a nested array field") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { items: [3]i32 };
        const B := Box{ .items = [_]i32{4, 5, 6} };
        pub const main := fn(): i32 {
            return B.items[2];
        };
    )") == 6);
}

TEST_CASE("Top-level string const with an explicit slice type is usable, not a raw i8*") {
    CHECK(helpers::compile_and_run(R"(
        const S: []u8 = "abcd";
        pub const main := fn(): i32 {
            return @intCast(i32, S.len);
        };
    )") == 4);
}

TEST_CASE("Top-level sentinel-string const passed to a function as a slice") {
    CHECK(helpers::compile_and_run(R"(
        const NAME: [:0]u8 = "hijkl";
        const first := fn(s: []u8): u8 {
            return s[0];
        };
        pub const main := fn(): i32 {
            return @as(i32, first(NAME));
        };
    )") == 'h');
}

TEST_CASE("Top-level sentinel-string const iterated at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const P: [:0]u8 = "ABC";
        pub const main := fn(): i32 {
            var n: i32 = 0;
            for (P) |c| { n += @as(i32, c); }
            return n - 100;
        };
    )") == ('A' + 'B' + 'C' - 100));
}

TEST_CASE("E2E WS-5: module-scope struct const with scalar and slice fields referenced by value") {
    CHECK(helpers::compile_and_run(R"(
        const Config := struct {
            id: i32,
            name: []u8,
        };
        const CFG := Config{ .id = 42, .name = "hello" };

        pub const main := fn(): i32 {
            const c := CFG;
            if (c.id != 42) { return 1; }
            if (c.name.len != 5) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("E2E WS-5: module-scope struct const referenced by pointer/ref") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const ORIGIN := Point{ .x = 10, .y = 20 };

        const get_x := fn(p: &Point): i32 {
            return p.x;
        };

        pub const main := fn(): i32 {
            const px := get_x(&ORIGIN);
            return px + ORIGIN.y;
        };
    )") == 30);
}

TEST_CASE("E2E WS-5: module-scope array of structs indexed and iterated") {
    CHECK(helpers::compile_and_run(R"(
        const Entry := struct { val: i32 };
        const ITEMS := [3]Entry{
            Entry{ .val = 10 },
            Entry{ .val = 20 },
            Entry{ .val = 30 },
        };

        pub const main := fn(): i32 {
            var sum: i32 = 0;
            for (ITEMS) |e| {
                sum += e.val;
            }
            return sum + ITEMS[1].val;
        };
    )") == 80);
}

TEST_CASE("E2E WS-5: module-scope union const lowering and matching") {
    CHECK(helpers::compile_and_run(R"(
        const Val := union {
            int_val: i32,
            bool_val: bool,
        };
        const DEF_VAL := Val{ .int_val = 99 };

        pub const main := fn(): i32 {
            return match (DEF_VAL) {
                .int_val => |v| v,
                .bool_val => 0,
            };
        };
    )") == 99);
}

TEST_CASE("E2E WS-5: module-scope struct with pointer field from int cast") {
    CHECK(helpers::compile_and_run(R"(
        const PtrHolder := struct {
            p: ^i32,
            offset: i32,
        };
        const HOLDER := PtrHolder{
            .p = nullptr,
            .offset = 7,
        };

        pub const main := fn(): i32 {
            if (HOLDER.p != nullptr) { return 1; }
            return HOLDER.offset;
        };
    )") == 7);
}

TEST_CASE("E2E WS-6: match arm return statement without semicolon") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := enum { A, B, C };
        const classify := fn(t: Tag): i32 {
            match (t) {
                .A => return 10,
                .B => return 20,
                .C => return 30,
            }
        };

        pub const main := fn(): i32 {
            return classify(.B);
        };
    )") == 20);
}

TEST_CASE("E2E WS-6: match arm break and continue statements without semicolon") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var sum: i32 = 0;
            var i: i32 = 0;
            while (i < 10) {
                i += 1;
                match (i) {
                    2 => continue,
                    5 => break,
                    _ => sum += i,
                }
            }
            return sum;
        };
    )") == (1 + 3 + 4));
}

TEST_CASE("E2E WS-6: pointer-to-bool coercion in @assert and @verify") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 123;
            const p: ^i32 = ^x;
            @assert(p);
            @assert(p, "pointer must be non-null");
            @verify(p);
            @verify(p, "pointer verified non-null");
            return *p;
        };
    )") == 123);
}

} // namespace ghoti::tests
