#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Operator type checking") {
    SECTION("Valid binary arithmetic succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(a: i32, b: i32): i32 {
                return a + b;
            };
        )");
    }

    SECTION("Adding booleans fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: bool, b: bool): void {
                const x := a + b;
            };
        )",
            sema::diagnostic{"Operator '+' cannot be applied to types 'bool' and 'bool'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 31UZ}});
    }

    SECTION("Mismatched numeric types in addition without cast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: i32, b: f32): void {
                const x := a + b;
            };
        )",
            sema::diagnostic{"Operator '+' cannot be applied to types 'i32' and 'f32'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 31UZ}});
    }

    SECTION("Comparing two differently-typed integer variables still fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: i32, b: usize): void {
                _ = a < b;
            };
        )",
            sema::diagnostic{"Relational operator cannot be applied to non-numeric or incompatible "
                             "types 'i32' and 'usize'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 24UZ}});
    }

    SECTION("Logical negation on non-boolean fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: i32): void {
                const x := !a;
            };
        )",
            sema::diagnostic{"Logical negation '!' requires a boolean operand; found 'i32'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 28UZ}});
    }

    SECTION("Unary negation on unsigned integer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: u32): void {
                const x := -a;
            };
        )",
            sema::diagnostic{
                "Unary negation '-' requires a signed integer or float operand; found 'u32'",
                sema::error::OPERATOR_TYPE_MISMATCH,
                std::pair{2UZ, 28UZ}});
    }

    SECTION("Bitwise negation on float fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: f32): void {
                const x := ~a;
            };
        )",
            sema::diagnostic{"Bitwise negation '~' requires an integer operand; found 'f32'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 28UZ}});
    }

    SECTION("Comparing two unions directly fails") {
        helpers::test_checker_fail(
            R"(
            const U := union { a: i32 };
            const f := fn(a: U, b: U): bool {
                return a == b;
            };
        )",
            sema::diagnostic{"Comparison operator cannot be applied to aggregate types "
                             "'U' and 'U'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{3UZ, 28UZ}});
    }

    SECTION("Comparing two structs directly fails") {
        helpers::test_checker_fail(
            R"(
            const S := struct { a: i32 };
            const f := fn(a: S, b: S): bool {
                return a == b;
            };
        )",
            sema::diagnostic{"Comparison operator cannot be applied to aggregate types "
                             "'S' and 'S'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{3UZ, 28UZ}});
    }
}

TEST_CASE("Discard statement evaluating expression") {
    helpers::type_check_and_verify(R"(
        pub const test_fn := fn(): i32 {
            var x: i32 = 5;
            _ = x + 10;
            return x;
        };
    )");
}

TEST_CASE("A post-resolution error inside an imported generic's monomorph is attributed to the "
          "defining module") {
    const auto check_attributed{[](std::string_view dep_gh, sema::error expected) {
        auto [ctx, idx]{helpers::type_check(
            R"(import "dep.gh" as dep; pub const main := fn(): i32 { return dep.g(5u32); };)",
            helpers::make_vector<helpers::mock_file>(
                helpers::mock_file{.path = "dep.gh", .source = dep_gh}))};

        // The root module still stops compiling, but prints nothing against its own source
        CHECK(ctx->root_mod.is_poisoned());
        if (const auto root_diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
            CHECK(root_diags->empty());
        }

        auto&       dep_module{*UNWRAP(ctx->manager.try_get_file_module("dep.gh"))};
        const auto& diags{UNWRAP(dep_module.diagnostics.as_opt<sema::diagnostics>())};
        REQUIRE_FALSE(diags.empty());
        for (const auto& d : diags) {
            CHECK(d.get_error() == expected);
            const auto loc{UNWRAP(d.to_formattable().location)};
            const auto [line, _]{dep_module.source.get_diagnostic_strings(loc)};
            CHECK(line != "<invalid line>");
        }
    }};

    SECTION("type checker") {
        check_attributed(R"(
            pub const g := fn(x: auto): i32 {
                const y: u16 = 2;
                return @intCast(x % y);
            };
        )",
                         sema::error::OPERATOR_TYPE_MISMATCH);
    }

    SECTION("GIR emission") {
        check_attributed(R"(
            pub const g := fn(x: auto): i32 {
                var a: u16 = 0;
                a = x;
                return a;
            };
        )",
                         sema::error::TYPE_MISMATCH);
    }
}

} // namespace ghoti::tests
