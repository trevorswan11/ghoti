#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// `@ptrFromArray` and `arr[lo..hi]` on a `[N]mut T` yield writable `^mut T` / `[]mut T`, so raw
// buffers can be written through `p[i] = v` and subslice element assignment. The array *binding*
// being `const` only prevents reseating it, not element writes.

TEST_CASE("`@ptrFromArray` on a `[N]mut T` yields a writable `^mut T`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [4uz]mut i32 = [4uz]mut i32{0, 0, 0, 0};
            var p := @ptrFromArray(a);
            p[0] = 40;
            p[3] = 2;
            return a[0] + a[3];
        };
    )") == 42);
}

TEST_CASE("`@ptrFromArray` stays writable even from a `const` binding") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: [2uz]mut i32 = [2uz]mut i32{0, 0};
            const p := @ptrFromArray(a);
            p[0] = 21;
            p[1] = 21;
            return a[0] + a[1];
        };
    )") == 42);
}

TEST_CASE("writing a buffer through a `^mut i32` parameter with `p[i] = v`") {
    CHECK(helpers::compile_and_run(R"(
        const fill := fn(p: ^mut i32, n: usize, v: i32): void {
            var i: usize = 0uz;
            loop {
                if (i == n) { break; }
                p[i] = v;
                i += 1uz;
            }
        };

        pub const main := fn(): i32 {
            var a: [6uz]mut i32 = [6uz]mut i32{0, 0, 0, 0, 0, 0};
            fill(@ptrFromArray(a), 6uz, 7);
            return a[0] + a[5];
        };
    )") == 14);
}

TEST_CASE("`arr[lo..hi]` on a `[N]mut T` is a writable subslice aliasing the array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [4uz]mut i32 = [4uz]mut i32{1, 2, 3, 4};
            var s := a[1..3];
            s[0] = 20;
            s[1] = 20;
            return a[1] + a[2] + a[0] + a[3];  // 20 + 20 + 1 + 4
        };
    )") == 45);
}

TEST_CASE("a `[N]T` (const elements) still yields read-only pointers/subslices") {
    // Reading is fine; the write would be a compile error, so only reads are exercised here.
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [4uz]i32 = [4uz]i32{10, 11, 12, 9};
            const p := @ptrFromArray(a);
            const s := a[0..4];
            return p[0] + s[3];
        };
    )") == 19);
}

} // namespace ghoti::tests
