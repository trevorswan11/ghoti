#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("module-scope `var` global: read and write through the bare name") {
    CHECK(helpers::compile_and_run(R"(
        var counter: i32 = 40;

        const bump := fn(): void { counter += 1; };

        pub const main := fn(): i32 {
            bump();
            bump();
            return counter;
        };
    )") == 42);
}

TEST_CASE("`var` static member of a struct via `Type.X` and `@this().X`") {
    CHECK(helpers::compile_and_run(R"(
        const Counter := struct {
            var count: i32 = 0;
            const inc := fn(): void { Counter.count += 1; };
            const value := fn(): i32 { return @this().count; };
        };

        pub const main := fn(): i32 {
            Counter.inc();
            Counter.inc();
            Counter.count += 40;
            return Counter.value();
        };
    )") == 42);
}

TEST_CASE("`var` static members of an enum and a union") {
    CHECK(helpers::compile_and_run(R"(
        const E := enum {
            a, b, c,
            var hits: i32 = 0;
            const K := 10;
            const tick := fn(): void { E.hits += 1; };
        };
        const U := union {
            x: i32,
            var calls: i32 = 0;
            const bump := fn(): void { @this().calls += 2; };
        };

        pub const main := fn(): i32 {
            E.tick(); E.tick();
            U.bump();
            E.hits += 28;
            return E.hits + E.K + U.calls;  // 30 + 10 + 2
        };
    )") == 42);
}

TEST_CASE("bare sibling static-member access in a struct member body") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            const K := 40;
            var count: i32 = 0;
            const use := fn(): i32 {
                count += 2;
                return K + count;
            };
        };

        pub const main := fn(): i32 { return S.use(); };
    )") == 42);
}

TEST_CASE("bare sibling static-member access in enum and union member bodies") {
    CHECK(helpers::compile_and_run(R"(
        const E := enum {
            a, b,
            const K := 30;
            var n: i32 = 0;
            const go := fn(): i32 { n += 12; return K + n; };
        };
        const U := union {
            x: i32,
            const M := 10;
            var c: i32 = 0;
            const run := fn(): i32 { c += 2; return M + c; };
        };

        pub const main := fn(): i32 {
            return E.go() + U.run() - 12;  // 42 + 12 - 12
        };
    )") == 42);
}

TEST_CASE("`var` struct global keeps its initializer and is mutable field-by-field") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        var anchor: Point = Point{ .x = 40, .y = 2 };

        pub const main := fn(): i32 {
            const before := anchor.x + anchor.y;  // 42
            anchor.y = 100;
            anchor.x = anchor.x - 40;
            return before + anchor.x + (anchor.y - 100);
        };
    )") == 42);
}

TEST_CASE("`var` array global keeps its initializer and is mutable element-by-element") {
    CHECK(helpers::compile_and_run(R"(
        var grid: [3uz]mut i32 = [3uz]mut i32{7, 8, 9};

        pub const main := fn(): i32 {
            grid[0] = grid[1] + grid[2] + 25;  // 42
            return grid[0];
        };
    )") == 42);
}

TEST_CASE("`var` struct global with a function-pointer field") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const VT := struct { step: fn(n: i32): i32, bias: i32 };
        var table: VT = VT{ .step = inc, .bias = 5 };

        pub const main := fn(): i32 {
            return table.step(36) + table.bias;  // 37 + 5
        };
    )") == 42);
}

} // namespace ghoti::tests
