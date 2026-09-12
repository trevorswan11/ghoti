#include <iostream>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("Plain functions produce byte-identical GIR across multiple emit passes") {
    constexpr std::string_view SOURCE{R"(
        const compute := fn(x: i32, y: i32): i32 {
            var sum: i32 = 0;
            var i: i32 = 0;
            while (i < x) {
                if (i % 2 == 0) {
                    sum = sum + y;
                } else {
                    sum = sum + 1;
                }
                i = i + 1;
            }
            return sum;
        };

        pub const main := fn(): i32 {
            return compute(5, 10);
        };
    )"};

    auto ctx{helpers::analyze("main.gh", std::cerr, SOURCE)};
    auto gir1{ctx->analyzer.emit_gir(ctx->root_mod)};
    auto gir2{ctx->analyzer.emit_gir(ctx->root_mod)};

    CHECK(!gir1.to_string().empty());
    CHECK(gir1.to_string() == gir2.to_string());

    const auto exit_code{helpers::compile_and_run(SOURCE)};
    CHECK(exit_code == 32);
}

TEST_CASE("Generic functions with multiple instantiations produce identical GIR") {
    constexpr std::string_view SOURCE{R"(
        const add := fn(T: type, a: T, b: T): T {
            return a + b;
        };

        const scale := fn(constexpr k: i32, x: i32): i32 {
            return x * k;
        };

        pub const main := fn(): i32 {
            const r1: i32 = add(i32, 10, 20);
            const r2: f64 = add(f64, 1.5, 2.5);
            const r3: i32 = scale(3, 7);
            const r4: i32 = scale(10, 4);
            return if (r1 == 30 and r2 == 4.0 and r3 == 21 and r4 == 40) 0 else 1;
        };
    )"};

    auto ctx{helpers::analyze("main.gh", std::cerr, SOURCE)};
    auto gir1{ctx->analyzer.emit_gir(ctx->root_mod)};
    auto gir2{ctx->analyzer.emit_gir(ctx->root_mod)};

    CHECK(!gir1.to_string().empty());
    CHECK(gir1.to_string() == gir2.to_string());

    const auto exit_code{helpers::compile_and_run(SOURCE)};
    CHECK(exit_code == 0);
}

TEST_CASE("Multiple modules sharing an interface impl produce identical GIR") {
    constexpr std::string_view IFACE_MOD{R"(
        pub const Greeter := interface {
            pub const greet := fn(&self): i32;
        };
    )"};

    constexpr std::string_view MOD_A{R"(
        import "iface.gh" as iface;
        pub const Human := struct { id: i32 };
        impl iface.Greeter for Human {
            pub const greet := fn(&self): i32 {
                return self.id * 10;
            };
        }
    )"};

    constexpr std::string_view MOD_B{R"(
        import "iface.gh" as iface;
        pub const Robot := struct { code: i32 };
        impl iface.Greeter for Robot {
            pub const greet := fn(&self): i32 {
                return self.code * 100;
            };
        }
    )"};

    constexpr std::string_view MAIN_SRC{R"(
        import "iface.gh" as iface;
        import "mod_a.gh" as a;
        import "mod_b.gh" as b;

        pub const main := fn(): i32 {
            const h: a.Human = .{ .id = 3 };
            const r: b.Robot = .{ .code = 4 };
            const g1: i32 = h.greet();
            const g2: i32 = r.greet();
            return if (g1 == 30 and g2 == 400) 0 else 1;
        };
    )"};

    const std::vector<mock_file> mocks{
        mock_file{"iface.gh", IFACE_MOD, "iface"},
        mock_file{"mod_a.gh", MOD_A, "mod_a"},
        mock_file{"mod_b.gh", MOD_B, "mod_b"},
    };

    auto ctx{helpers::analyze("main.gh",
                              std::cerr,
                              MAIN_SRC,
                              mock_file{"iface.gh", IFACE_MOD, "iface"},
                              mock_file{"mod_a.gh", MOD_A, "mod_a"},
                              mock_file{"mod_b.gh", MOD_B, "mod_b"})};

    auto gir1{ctx->analyzer.emit_gir(ctx->root_mod)};
    auto gir2{ctx->analyzer.emit_gir(ctx->root_mod)};

    CHECK(!gir1.to_string().empty());
    CHECK(gir1.to_string() == gir2.to_string());

    const auto exit_code{helpers::compile_and_run(MAIN_SRC, mocks)};
    CHECK(exit_code == 0);
}

TEST_CASE("`fn(...): type` constructors with const members produce identical GIR") {
    constexpr std::string_view SOURCE{R"(
        const Pair := fn(T: type): type {
            return struct {
                first: T,
                second: T,

                pub const DEFAULT_TAG: i32 = 42;
                pub const swap := fn(&self): @This() {
                    return .{ .first = self.second, .second = self.first };
                };
            };
        };

        using P = Pair(i32);

        pub const main := fn(): i32 {
            const p1: P = .{ .first = 10, .second = 20 };
            const s1: P = p1.swap();
            return if (s1.first == 20 and s1.second == 10 and P.DEFAULT_TAG == 42) 0 else 1;
        };
    )"};

    auto ctx{helpers::analyze("main.gh", std::cerr, SOURCE)};
    auto gir1{ctx->analyzer.emit_gir(ctx->root_mod)};
    auto gir2{ctx->analyzer.emit_gir(ctx->root_mod)};

    CHECK(!gir1.to_string().empty());
    CHECK(gir1.to_string() == gir2.to_string());

    const auto exit_code{helpers::compile_and_run(SOURCE)};
    CHECK(exit_code == 0);
}

} // namespace ghoti::tests
