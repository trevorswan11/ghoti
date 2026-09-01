#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view K1{R"(
    pub var counter: i32 = 10;
    pub const bump := fn(): void { counter = counter + 1; };
    pub const get  := fn(): i32  { return counter; };
    pub const tag  := fn(): i32  { return 3; };
)"};

constexpr std::string_view K2{R"(
    pub var counter: i32 = 50;
    pub const bump := fn(): void { counter = counter + 10; };
    pub const get  := fn(): i32  { return counter; };
    pub const tag  := fn(): i32  { return 7; };
)"};

} // namespace

TEST_CASE("E2E: same-named `pub const fn` in two modules stay distinct") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "k1.gh" as k1;
            import "k2.gh" as k2;

            pub const main := fn(): i32 {
                return k1::tag() * 10 + k2::tag();   // 37
            };
        )",
        {
            helpers::mock_file{"k1.gh", K1, "k1"},
            helpers::mock_file{"k2.gh", K2, "k2"},
        })};

    CHECK(exit_code == 37);
}

TEST_CASE("E2E: same-named `pub var` in two modules have independent storage") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "k1.gh" as k1;
            import "k2.gh" as k2;

            pub const main := fn(): i32 {
                k1::bump();
                k2::bump();
                k2::bump();
                return k1::get() + k2::get();   // (10+1) + (50+20) = 81
            };
        )",
        {
            helpers::mock_file{"k1.gh", K1, "k1"},
            helpers::mock_file{"k2.gh", K2, "k2"},
        })};
    CHECK(exit_code == 81);
}

TEST_CASE("E2E: a module-scope alias of a colliding cross-module function") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "k1.gh" as k1;
            import "k2.gh" as k2;

            const first  := k1::tag;
            const second := k2::tag;

            pub const main := fn(): i32 {
                return first() * 10 + second();   // 37
            };
        )",
        {
            helpers::mock_file{"k1.gh", K1, "k1"},
            helpers::mock_file{"k2.gh", K2, "k2"},
        })};
    CHECK(exit_code == 37);
}

TEST_CASE("E2E: two structs in one module with a same-named static method") {
    CHECK(helpers::compile_and_run(R"(
        const A := struct {
            n: i32,
            const make := fn(v: i32): @this() { return .{ .n = v }; };
        };
        const B := struct {
            m: i32,
            const make := fn(v: i32): @this() { return .{ .m = v * 3 }; };
        };

        pub const main := fn(): i32 {
            const a := A.make(10);
            const b := B.make(4);
            return a.n + b.m;   // 10 + 12 = 22
        };
    )") == 22);
}

TEST_CASE("E2E: two structs in one module with a same-named `^self` method") {
    CHECK(helpers::compile_and_run(R"(
        const A := struct {
            n: i32,
            const make := fn(v: i32): @this() { return .{ .n = v }; };
            const value := fn(^self): i32 { return self.n + 1; };
        };
        const B := struct {
            n: i32,
            const make := fn(v: i32): @this() { return .{ .n = v }; };
            const value := fn(^self): i32 { return self.n * 2; };
        };

        pub const main := fn(): i32 {
            const a := A.make(10);
            const b := B.make(10);
            return a.value() + b.value();   // 11 + 20 = 31
        };
    )") == 31);
}

namespace {

constexpr std::string_view SHAPE_A{R"(
    pub const Shape := struct {
        k: i32,
        pub const make := fn(v: i32): @this() { return .{ .k = v }; };
        pub const area := fn(^self): i32 { return self.k + 1; };
    };
)"};

constexpr std::string_view SHAPE_B{R"(
    pub const Shape := struct {
        k: i32,
        pub const make := fn(v: i32): @this() { return .{ .k = v }; };
        pub const area := fn(^self): i32 { return self.k * 5; };
    };
)"};

} // namespace

TEST_CASE("E2E: same-named method on same-named struct in two modules") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sa.gh" as sa;
            import "sb.gh" as sb;

            pub const main := fn(): i32 {
                const a := sa::Shape.make(4);   // area -> 5
                const b := sb::Shape.make(4);   // area -> 20
                const viaObj := a.area();
                const viaType := sb::Shape.area(^b);
                return viaObj + viaType;        // 5 + 20 = 25
            };
        )",
        {
            helpers::mock_file{"sa.gh", SHAPE_A, "sa"},
            helpers::mock_file{"sb.gh", SHAPE_B, "sb"},
        })};
    CHECK(exit_code == 25);
}

TEST_CASE("E2E: a module function and a struct method sharing a name do not cross-talk") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(v: i32): i32 { return v + 100; };

        const Widget := struct {
            n: i32,
            const make := fn(v: i32): @this() { return .{ .n = v * 2 }; };
        };

        pub const main := fn(): i32 {
            const w := Widget.make(9);   // n = 18
            return make(7) + w.n;        // 107 + 18 = 125
        };
    )") == 125);
}

namespace {

constexpr std::string_view EXPORTER{R"(
    export("ghoti_exported_answer") const answer := fn(): i32 { return 42; };
    extern("ghoti_fake_libc", "answer") const raw_answer: fn(): i32;
    pub const relay := fn(): i32 { return answer(); };
)"};

} // namespace

TEST_CASE("E2E: an `export`ed name still resolves alongside a colliding internal one") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "exp.gh" as exp;

            const answer := fn(): i32 { return 7; };

            pub const main := fn(): i32 {
                return answer() * 10 + exp::relay();   // 7*10 + 42 = 112
            };
        )",
        {helpers::mock_file{"exp.gh", EXPORTER, "exp"}})};
    CHECK(exit_code == 112);
}

} // namespace ghoti::tests
