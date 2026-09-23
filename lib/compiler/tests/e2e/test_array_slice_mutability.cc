#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Indexing an array literal rvalue") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return [3]i32{7, 8, 9}[0];
        };
    )") == 7);
}

TEST_CASE("Dynamic index into an array literal rvalue") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var i: usize = 1;
            return [3]i32{7, 8, 9}[i];
        };
    )") == 8);
}

TEST_CASE("Array literal passed as a by-value array argument") {
    CHECK(helpers::compile_and_run(R"(
        const first := fn(a: [3]i32): i32 {
            return a[0];
        };
        pub const main := fn(): i32 {
            return first([3]i32{7, 8, 9});
        };
    )") == 7);
}

TEST_CASE("For loop by value over a local array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            var sum: i32 = 0;
            for (arr) |v| {
                sum = sum + v;
            }
            return sum;
        };
    )") == 6);
}

TEST_CASE("Local array variable read back correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{7, 8, 9};
            var x: i32 = arr[0];
            return x;
        };
    )") == 7);
}

TEST_CASE("Array decayed to an explicit slice-typed local") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            var sl: []i32 = arr;
            return sl[1];
        };
    )") == 2);
}

TEST_CASE("Array built from runtime (non-constant) values") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(a: i32, b: i32, c: i32): i32 {
            var arr := [3]i32{a, b, c};
            return arr[0];
        };
        pub const main := fn(): i32 {
            return make(7, 8, 9);
        };
    )") == 7);
}

TEST_CASE("Struct literal control case") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const make := fn(a: i32, b: i32): i32 {
            var p := Point{ .x = a, .y = b };
            return p.x;
        };
        pub const main := fn(): i32 {
            return make(7, 8);
        };
    )") == 7);
}

TEST_CASE("Struct field with a sized array type") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { items: [3]i32 };
        pub const main := fn(): i32 {
            var b := Box{ .items = [3]i32{7, 8, 9} };
            return b.items[0];
        };
    )") == 7);
}

TEST_CASE("Tagged union scalar field initializer") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .val = 7 };
            return 0;
        };
    )") == 0);
}

TEST_CASE("Tagged union field read") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .val = 42 };
            return u.val;
        };
    )") == 42);
}

TEST_CASE("Tagged union bool field") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        pub const main := fn(): i32 {
            var u := U{ .flag = true };
            return if (u.flag) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("Tagged union field with an array type") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { items: [3]i32, flag: bool };
        pub const main := fn(): i32 {
            var u := U{ .items = [3]i32{7, 8, 9} };
            return u.items[0];
        };
    )") == 7);
}

TEST_CASE("Mutable slice parameter mutates the caller's array (var)") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )") == 11);
}

TEST_CASE("Mutable slice parameter mutates the caller's array (const)") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            const slice := [3]mut i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )") == 11);
}

TEST_CASE("mut array decays to a mut slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const first := fn(arr: []mut i32): i32 {
            return arr[0];
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{1, 2, 3};
            return first(slice);
        };
    )") == 1);
}

TEST_CASE("mut array implicitly coerces to a const slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const read_only := fn(arr: []i32): i32 {
            return arr[0];
        };
        pub const main := fn(): i32 {
            var slice := [3]mut i32{5, 6, 7};
            return read_only(slice);
        };
    )") == 5);
}

TEST_CASE("constCast on a const array preserves identity for mutation") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 100;
        };
        pub const main := fn(): i32 {
            var slice := [3]i32{1, 2, 3};
            bump(@constCast(slice));
            return slice[0];
        };
    )") == 101);
}

TEST_CASE("Directly writing to a mut array's elements") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]mut i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )") == 5);
}

TEST_CASE("mut volatile scalar reads and writes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var v: mut volatile i32 = 42;
            v = v + 1;
            return v;
        };
    )") == 43);
}

TEST_CASE("Passing a const array where a mut slice is required is rejected") {
    helpers::expect_compile_error(R"(
        const bump := fn(arr: []mut i32): void {
            arr[0] = arr[0] + 10;
        };
        pub const main := fn(): i32 {
            var slice := [3]i32{1, 2, 3};
            bump(slice);
            return slice[0];
        };
    )");
}

TEST_CASE("Writing to a const array's element is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const arr := [3]i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )");
}

TEST_CASE("Writing to a var array's element without mut is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            arr[0] = 5;
            return arr[0];
        };
    )");
}

TEST_CASE("Rebinding a const scalar is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x := 5;
            x = 6;
            return x;
        };
    )");
}

TEST_CASE("Indexing a slice parameter of a nested function, passed an existing slice") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr: [2uz]mut i32 = [2uz]mut i32{7, 8};
            var sl: []mut i32 = arr;
            const readit := fn(s: []mut i32): i32 {
                return s[0];
            };
            return readit(sl);
        };
    )") == 7);
}

TEST_CASE("A `var` slice binding's `.len` field is directly assignable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: []mut u8 = [_]mut u8{1, 2, 3, 4, 5, 6};
            a.len = 2;
            return @intCast(i32, a.len);
        };
    )") == 2);
}

TEST_CASE("A `var` slice binding's `.len` field is assignable even with immutable elements") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: []u8 = [_]u8{1, 2, 3, 4, 5, 6};
            a.len = 3;
            return @intCast(i32, a.len);
        };
    )") == 3);
}

TEST_CASE("A `const` slice binding's `.len` field is still rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const a: []mut u8 = [_]mut u8{1, 2, 3, 4, 5, 6};
            a.len = 2;
            return @intCast(i32, a.len);
        };
    )");
}

TEST_CASE("Const-element arrays copy into mutable-element storage") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(): [3]u8 { return [3]u8{4, 5, 6}; };
        const sum := fn(a: [3]mut u8): i32 {
            a[0] = 0;
            return @as(i32, a[0]) + @as(i32, a[1]) + @as(i32, a[2]);
        };
        pub const main := fn(): i32 {
            const arr: [3]u8 = .{1, 2, 3};
            var m: [3]mut u8 = arr;
            var n: [3]mut u8 = [3]u8{1, 2, 3};
            var k: [3]mut u8 = make();
            m[0] = 10;
            n[0] = 20;
            k[0] = 30;
            if (arr[0] != 1) { return 1; }
            if (sum(arr) != 5 or arr[0] != 1) { return 2; }
            return @as(i32, m[0]) + @as(i32, n[0]) + @as(i32, k[0]) +
                   @as(i32, m[2]) + @as(i32, k[2]);
        };
    )") == 10 + 20 + 30 + 3 + 6);
}

TEST_CASE("Array copy of pointer and slice elements still aliases their pointees") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: u8 = 1;
            const p: ^mut u8 = ^mut x;
            const arr: [2]^mut u8 = .{p, p};
            var m: [2]mut ^u8 = arr;
            var q: [2]mut ^mut u8 = arr;
            q[1][0] = 7;
            const s: [2][]u8 = .{"ab", "cd"};
            var t: [2]mut []u8 = s;
            t[0] = "zz";
            return @as(i32, m[0][0]) + @as(i32, t[0][0]) + @as(i32, s[0][0]);
        };
    )") == 7 + 'z' + 'a');
}

TEST_CASE("A reference to an array coerces to a slice through the reference") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const x: [2]u8 = .{1, 2};
            const t: []u8 = &x;
            return @as(i32, t[1]) + @as(i32, @intCast(t.len)) * 10;
        };
    )") == 22);
}

TEST_CASE("A reference to a nested array coerces to a slice of arrays") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const m: [2][2]u8 = .{.{1, 2}, .{3, 4}};
            const u: [][2]u8 = &m;
            return @as(i32, u[1][0]) + @as(i32, u[1][1]) + @as(i32, @intCast(u.len)) * 10;
        };
    )") == 27);
}

TEST_CASE("A mut reference to an array coerces to a mut slice aliasing the array") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(s: []mut i32): void {
            s[0] = s[0] + 10;
        };
        pub const main := fn(): i32 {
            var arr := [3]mut i32{1, 2, 3};
            bump(&mut arr);
            const r: &mut [3]mut i32 = &mut arr;
            const sl: []mut i32 = r;
            sl[2] = 30;
            return arr[0] + arr[2];
        };
    )") == 11 + 30);
}

TEST_CASE("A reference to an array stays an array reference outside a slice context") {
    CHECK(helpers::compile_and_run(R"(
        const second := fn(a: &[3]i32): i32 {
            return a[1];
        };
        pub const main := fn(): i32 {
            const arr := [3]i32{4, 5, 6};
            const r := &arr;
            const copy: [3]i32 = r;
            return second(r) + copy[2] + @as(i32, @intCast(r.len));
        };
    )") == 5 + 6 + 3);
}

TEST_CASE("A pointer to an array does not coerce to a slice") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x: [2]u8 = .{1, 2};
            const t: []u8 = ^x;
            return @as(i32, t[1]);
        };
    )");
}

} // namespace ghoti::tests
