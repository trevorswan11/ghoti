#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("static member constant read via the type name and `@this()`") {
    CHECK(helpers::compile_and_run(R"(
        const Cfg := struct {
            const LIMIT := 40;
            const bump := fn(): i32 { return @this().LIMIT + 2; };
        };

        pub const main := fn(): i32 {
            return Cfg.LIMIT + (Cfg.bump() - Cfg.LIMIT);
        };
    )") == 42);
}

TEST_CASE("address of a scalar static member constant") {
    CHECK(helpers::compile_and_run(R"(
        const Cfg := struct {
            const BASE := 21;
            const via_this := fn(): i32 { const p := ^@this().BASE; return *p; };
            const via_name := fn(): i32 { const p := ^Cfg.BASE; return *p; };
        };

        pub const main := fn(): i32 {
            return Cfg.via_this() + Cfg.via_name();
        };
    )") == 42);
}

TEST_CASE("address of an aggregate static member constant (vtable singleton)") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const VTable := struct { step: fn(n: i32): i32 };

        const Widget := struct {
            const table := VTable{ .step = inc };
            const run := fn(): i32 {
                const vt := ^Widget.table;
                return vt.step(41);
            };
            const run_this := fn(): i32 {
                const vt := ^@this().table;
                return vt.step(41);
            };
        };

        pub const main := fn(): i32 {
            return Widget.run() + Widget.run_this() - 42;
        };
    )") == 42);
}

TEST_CASE("reference to a global constant aggregate, then field access") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const ORIGIN := Point{ .x = 40, .y = 2 };

        pub const main := fn(): i32 {
            const r := &ORIGIN;
            return r.x + r.y;
        };
    )") == 42);
}

} // namespace ghoti::tests
