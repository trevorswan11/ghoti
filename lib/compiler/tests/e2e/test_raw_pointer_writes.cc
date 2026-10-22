#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// `arr.ptr` and `arr[lo..hi]` on a `[N]mut T` yield writable `^mut T` / `[]mut T`, so raw
// buffers can be written through `p[i] = v` and subslice element assignment. The array *binding*
// being `let` only prevents reseating it, not element writes.

TEST_CASE("`.ptr` on a `[N]mut T` yields a writable `^mut T`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: [4uz]mut i32 = [4uz]mut i32{0, 0, 0, 0};
            let mut p = a.ptr;
            p[0] = 40;
            p[3] = 2;
            return a[0] + a[3];
        };
    )") == 42);
}

TEST_CASE("`.ptr` stays writable even from a `const` binding") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: [2uz]mut i32 = [2uz]mut i32{0, 0};
            let p = a.ptr;
            p[0] = 21;
            p[1] = 21;
            return a[0] + a[1];
        };
    )") == 42);
}

TEST_CASE("writing a buffer through a `^mut i32` parameter with `p[i] = v`") {
    CHECK(helpers::compile_and_run(R"(
        const fill = fn(p: ^mut i32, n: usize, v: i32): void {
            let mut i: usize = 0uz;
            loop {
                if (i == n) { break; }
                p[i] = v;
                i += 1uz;
            }
        };

        pub const main = fn(): i32 {
            let mut a: [6uz]mut i32 = [6uz]mut i32{0, 0, 0, 0, 0, 0};
            fill(a.ptr, 6uz, 7);
            return a[0] + a[5];
        };
    )") == 14);
}

TEST_CASE("`arr[lo..hi]` on a `[N]mut T` is a writable subslice aliasing the array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: [4uz]mut i32 = [4uz]mut i32{1, 2, 3, 4};
            let mut s = a[1..3];
            s[0] = 20;
            s[1] = 20;
            return a[1] + a[2] + a[0] + a[3];  // 20 + 20 + 1 + 4
        };
    )") == 45);
}

TEST_CASE("a `[N]T` (const elements) still yields read-only pointers/subslices") {
    // Reading is fine; the write would be a compile error, so only reads are exercised here.
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: [4uz]i32 = [4uz]i32{10, 11, 12, 9};
            let p = a.ptr;
            let s = a[0..4];
            return p[0] + s[3];
        };
    )") == 19);
}

TEST_CASE("`p[i]` steps over a pointer's struct elements") {
    CHECK(helpers::compile_and_run(R"(
        const Pair = extern struct { a: usize, b: usize };
        const fill = fn(p: ^mut Pair, n: usize): void {
            for (0..n) |i| {
                p[i] = .{ .a = i, .b = 10 * i };
            }
        };
        pub const main = fn(): i32 {
            let mut pairs: [3]mut Pair = undefined;
            fill(pairs.ptr, 3);
            let p = pairs.ptr;
            return @intCast(i32, p[2].b + p[1].a + pairs[2].a);  // 20 + 1 + 2
        };
    )") == 23);
}

TEST_CASE("A statement led by a block-like expression ends at its closing brace") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 1;
            let p: ^mut i32 = ^mut x;
            if (x != 1) { x = 100; }
            *p = 7;
            while (x > 100) { x -= 1; }
            for (0..1) |_| { x += 1; }
            *p += 2;
            match (x) { 10 => { x += 1; }, _ => {} }
            *p += 3;
            return x;
        };
    )") == 14);
}

} // namespace ghoti::tests
