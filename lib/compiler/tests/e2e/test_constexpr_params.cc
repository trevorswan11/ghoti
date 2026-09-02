#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("constexpr parameter: distinct values monomorphize to distinct behavior") {
    CHECK(helpers::compile_and_run(R"(
        const shifted := fn(constexpr by: i32, x: i32): i32 {
            return x + by;
        };

        pub const main := fn(): i32 {
            return shifted(10, 1) + shifted(100, 0);
        };
    )") == 111);
}

TEST_CASE("constexpr parameter: reused value hits the instantiation cache once") {
    CHECK(helpers::compile_and_run(R"(
        const scale := fn(constexpr k: i32, x: i32): i32 {
            return x * k;
        };

        pub const main := fn(): i32 {
            return scale(3, 2) + scale(3, 5) + scale(4, 1);
        };
    )") == 6 + 15 + 4);
}

TEST_CASE("constexpr parameter: folds inside `if constexpr` in the body") {
    CHECK(helpers::compile_and_run(R"(
        const clamp_double := fn(constexpr n: i32): i32 {
            if constexpr (n > 100) {
                @compileError("n is too large");
            }
            return n * 2;
        };

        pub const main := fn(): i32 {
            return clamp_double(21);
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: a struct value is usable in the body") {
    CHECK(helpers::compile_and_run(R"(
        const P := struct { x: i32, y: i32 };
        const dot := fn(constexpr p: P): i32 { return p.x * 10 + p.y; };

        pub const main := fn(): i32 {
            return dot(P{ .x = 4, .y = 2 });
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: `auto` is both value- and type-generic") {
    CHECK(helpers::compile_and_run(R"(
        const twice := fn(constexpr v: auto): i32 { return @as(i32, v) + @as(i32, v); };

        pub const main := fn(): i32 {
            return twice(15) + twice(6);
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: a compile-time function reference") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(x: i32): i32 { return x + 1; };

        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 {
            return f(f(v));
        };

        pub const main := fn(): i32 {
            return apply(inc, 40);
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: a local function reference") {
    CHECK(helpers::compile_and_run(R"(
        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 { return f(f(v)); };

        pub const main := fn(): i32 {
            const dec := fn(x: i32): i32 { return x - 1; };
            const inc := fn(x: i32): i32 { return x + 1; };
            return apply(dec, 22) + apply(inc, 18);
        };
    )") == 40);
}

TEST_CASE("constexpr parameter: a closure with compile-time captures") {
    CHECK(helpers::compile_and_run(R"(
        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 { return f(v); };

        pub const main := fn(): i32 {
            const k: i32 = 10;
            const addk := fn(x: i32): i32 { return x + k; };
            return apply(addk, 32);
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: an anonymous capturing closure literal") {
    CHECK(helpers::compile_and_run(R"(
        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 { return f(f(v)); };

        pub const main := fn(): i32 {
            const step: i32 = 3;
            return apply(fn(x: i32): i32 { return x + step; }, 36);
        };
    )") == 42);
}

TEST_CASE("constexpr closure: distinct captured values monomorphize apart") {
    CHECK(helpers::compile_and_run(R"(
        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 { return f(v); };

        pub const main := fn(): i32 {
            const a: i32 = 10;
            const b: i32 = 100;
            const addA := fn(x: i32): i32 { return x + a; };
            const addB := fn(x: i32): i32 { return x + b; };
            return addA(0) + addB(0) + apply(addA, 1) + apply(addB, 2);
        };
    )") == 10 + 100 + 11 + 102);
}

TEST_CASE("constexpr closure: invoked at comptime and runtime in the body") {
    CHECK(helpers::compile_and_run(R"(
        const use := fn(constexpr f: fn(n: i32): i32, v: i32): i32 {
            if constexpr (f(1) == 5) { return f(v) + 100; }
            return f(v);
        };

        pub const main := fn(): i32 {
            const add4 := fn(x: i32): i32 { return x + 4; };
            return use(add4, 38);
        };
    )") == 142);
}

TEST_CASE("constexpr closure: a runtime capture is rejected") {
    helpers::expect_compile_error(R"(
        const apply := fn(constexpr f: fn(n: i32): i32, v: i32): i32 { return f(v); };

        pub const main := fn(): i32 {
            var k: i32 = 10;
            const addk := fn(x: i32): i32 { return x + k; };
            return apply(addk, 1);
        };
    )");
}

TEST_CASE("constexpr parameter: forward-referenced constexpr argument") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { return take(give()); };
        const give := fn(): i32 { return 42; };
        const take := fn(constexpr n: i32): i32 { return n; };
    )") == 42);
}

TEST_CASE("constexpr parameter: sizes a type in the body, per value") {
    CHECK(helpers::compile_and_run(R"(
        const bytes := fn(constexpr n: usize): i32 {
            return @as(i32, @sizeOf([n]i32));
        };

        pub const main := fn(): i32 {
            return bytes(2uz) + bytes(5uz);
        };
    )") == 8 + 20);
}

TEST_CASE("constexpr parameter: a non-constant argument is rejected") {
    helpers::expect_compile_error(R"(
        const need_const := fn(constexpr n: i32): i32 {
            return n;
        };

        pub const main := fn(): i32 {
            var x: i32 = 7;
            return need_const(x);
        };
    )");
}

TEST_CASE("constexpr parameter: @compileError fires only for the offending instantiation") {
    helpers::expect_compile_error(R"(
        const bounded := fn(constexpr n: i32): i32 {
            if constexpr (n > 100) {
                @compileError("n is too large");
            }
            return n;
        };

        pub const main := fn(): i32 {
            return bounded(5) + bounded(500);
        };
    )");
}

TEST_CASE("constexpr parameter: `constexpr` on a `type` parameter is redundant") {
    helpers::test_resolver_fail(
        "const zero := fn(constexpr t: type): t { return 0; };",
        sema::diagnostic{"'constexpr' is redundant on a parameter of type 'type'; type values "
                         "are always compile-time known",
                         sema::error::REDUNDANT_CONSTEXPR,
                         std::pair{0UZ, 27UZ}});
}

TEST_CASE("constexpr parameter: an all-constexpr signature still works") {
    CHECK(helpers::compile_and_run(R"(
        const combine := fn(constexpr a: i32, constexpr b: i32): i32 { return a * b; };

        pub const main := fn(): i32 {
            return combine(6, 7);
        };
    )") == 42);
}

} // namespace ghoti::tests
