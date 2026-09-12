#include <catch2/catch_test_macros.hpp>

#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("if constexpr: a folded non-generic condition resolves only the live arm") {
    SECTION("Dead alternate is never name-resolved") {
        auto [ctx, idx]{helpers::resolve_and_check(
            "const chosen := if constexpr (true) 7 else undeclared_in_dead_arm;")};

        const auto [sym, _, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("chosen", idx)};
        CHECK(type == ctx->get_type(sema::type_kind::CONSTEXPR_INT));
    }

    SECTION("Node takes the alternate's type, not the consequence's") {
        auto [ctx,
              idx]{helpers::resolve_and_check("const chosen := if constexpr (false) 7 else true;")};

        const auto [sym, _, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("chosen", idx)};
        CHECK(type == ctx->get_type(sema::type_kind::BOOL));
    }

    SECTION("@compileError in the pruned arm stays silent") {
        CHECK(helpers::resolve_diags(
                  "const f := fn(): i32 { if constexpr (false) { @compileError(\"dead\"); } "
                  "else { return 0; } };")
                  .codes.empty());
    }

    SECTION("@compileError in the live arm still fires") {
        const auto [codes, _]{helpers::resolve_diags(
            "const f := fn(): i32 { if constexpr (true) { @compileError(\"live\"); } "
            "else { return 0; } };")};
        REQUIRE_FALSE(codes.empty());
        CHECK(codes[0] == sema::error::COMPILE_ERROR_REACHED);
    }
}

TEST_CASE("if constexpr: a non-foldable condition still resolves both arms") {
    const auto [codes, _]{helpers::resolve_diags(
        "const f := fn(param: bool): i32 { if constexpr (param) { return 1; } "
        "else { return still_undeclared; } };")};
    REQUIRE_FALSE(codes.empty());
    CHECK(codes[0] == sema::error::UNDECLARED_IDENTIFIER);
}

TEST_CASE("if constexpr: a generic body prunes per instantiation") {
    CHECK(helpers::resolve_diags(R"(
        const pick := fn(x: auto): i32 {
            if constexpr (@typeOf(x) == u8) {
                return undeclared_only_in_dead_generic_arm(x);
            } else {
                return 32;
            }
        };
        const r := pick(0i64);
    )")
              .codes.empty());
}

} // namespace ghoti::tests
