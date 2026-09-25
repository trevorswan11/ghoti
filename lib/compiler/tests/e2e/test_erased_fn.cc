#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("an erased `fn(...)` local holds a plain function or a capturing closure") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32): i32 { return a + b; };
        pub const main := fn(): i32 {
            var base: i32 = 20;
            const near := fn(a: i32, b: i32): i32 { return a + b + base; };
            var f: fn(a: i32, b: i32): i32 = add;
            const x := f(1, 2);
            f = near;
            return x + f(3, 4);
        };
    )") == 3 + 27);
}

TEST_CASE("an erased `fn(...)` struct field stores a capturing closure") {
    CHECK(helpers::compile_and_run(R"(
        const Handler := struct {
            on_event: fn(code: i32): i32,
            pub const fire := fn(&self, code: i32): i32 { return self.on_event(code); };
        };
        pub const main := fn(): i32 {
            var seen: i32 = 0;
            const record := fn(code: i32): i32 {
                seen += code;
                return seen;
            };
            const h := Handler{ .on_event = record };
            _ = h.fire(10);
            return h.fire(5) + seen;
        };
    )") == 15 + 15);
}

TEST_CASE("a non-generic function accepts a closure literal through an erased parameter") {
    CHECK(helpers::compile_and_run(R"(
        const twice := fn(f: fn(n: i32): i32, v: i32): i32 { return f(f(v)); };
        pub const main := fn(): i32 {
            var step: i32 = 3;
            return twice(fn(n: i32): i32 { return n + step; }, 1) + twice(fn(n: i32): i32 {
                return n * 2;
            }, 5);
        };
    )") == 7 + 20);
}

TEST_CASE("an erased `fn(...)` is returned and called later") {
    CHECK(helpers::compile_and_run(R"(
        const IntFn := fn(n: i32): i32;
        const inc := fn(n: i32): i32 { return n + 1; };
        const pick := fn(double: bool): IntFn {
            if (double) { return fn(n: i32): i32 { return n * 2; }; }
            return inc;
        };
        pub const main := fn(): i32 {
            const a := pick(true);
            const b := pick(false);
            return a(20) + b(1);
        };
    )") == 40 + 2);
}

TEST_CASE("returning a capturing closure as an erased `fn(...)` is rejected") {
    for (const auto* kw : {"fn", "move fn"}) {
        CAPTURE(kw);
        CHECK(helpers::raised(fmt::format(R"(
        const IntFn := fn(n: i32): i32;
        const make := fn(base: i32): IntFn {{
            return {}(n: i32): i32 {{ return n + base; }};
        }};
    )",
                                          kw),
                              sema::error::ILLEGAL_CLOSURE_ESCAPE));
    }
}

TEST_CASE("a capturing closure is still returned by its own type through `auto`") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(base: i32): auto {
            return move fn(n: i32): i32 { return n + base; };
        };
        pub const main := fn(): i32 {
            const add_ten := make(10);
            const erased: fn(n: i32): i32 = add_ten;
            return add_ten(1) + erased(2);
        };
    )") == 11 + 12);
}

TEST_CASE("a `^fn(...)` is nullable and compares against `nullptr`") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        pub const main := fn(): i32 {
            var p: ^fn(n: i32): i32 = nullptr;
            var score: i32 = 0;
            if (p == nullptr) { score += 1; }
            p = inc;
            if (p != nullptr) { score += 10; }
            return score + p(4) + (*p)(5);
        };
    )") == 11 + 5 + 6);
}

TEST_CASE("an erased `fn(...)` is two pointers wide and an `extern fn` is one") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const erased := @sizeOf(fn(n: i32): i32);
            const thin := @sizeOf(extern fn(n: i32): i32);
            return @intCast(i32, erased / thin);
        };
    )") == 2);
}

TEST_CASE("a module-scope erased `fn(...)` global is initialized from a function") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const Table := struct { op: fn(n: i32): i32 };
        const table := Table{ .op = inc };
        var current: fn(n: i32): i32 = inc;
        pub const main := fn(): i32 {
            return table.op(40) + current(0);
        };
    )") == 41 + 1);
}

TEST_CASE("an erased `fn(...)` never flows back into a thin `extern fn(...)`") {
    helpers::expect_compile_error(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const run := fn(f: fn(n: i32): i32): i32 {
            const thin: extern fn(n: i32): i32 = f;
            return thin(1);
        };
        pub const main := fn(): i32 { return run(inc); };
    )");
}

TEST_CASE("an erased `fn(...)` cannot be an `extern struct` field") {
    CHECK(helpers::raised("const S := extern struct { cb: fn(n: i32): i32 };",
                          sema::error::ILLEGAL_REFERENCE_FIELD));
    helpers::resolve_and_check("const S := extern struct { cb: extern fn(n: i32): i32 };");
}

} // namespace ghoti::tests
