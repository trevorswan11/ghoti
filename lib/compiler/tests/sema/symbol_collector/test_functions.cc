#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Function hollow types") {
    auto [ctx, idx]{
        helpers::collect_and_check("const a := fn(&self, c: type): void { const foo := bar; };")};
    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == 2);

    const auto [sym, sym_data, node_data]{
        ctx->get_ast_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    CHECK(sym.get_kind_opt() == sema::symbol_kind::CALLABLE);

    const auto& fn_type{UNWRAP(ctx->root_mod.get_sema_type_opt(*node_data.value))};
    CHECK(fn_type == ctx->get_type(sema::type_kind::FUNCTION, 1));

    REQUIRE(UNWRAP(registry.get_from_opt(1, "self")).get_data().as_opt<syms::self_parameter>());
    REQUIRE(UNWRAP(registry.get_from_opt(1, "c")).get_data().as_opt<syms::parameter>());
    ctx->test_common_decl_collection(1);
}

TEST_CASE("Well-placed function control-flow statements") {
    helpers::collect_and_check("pub const main := fn(args: [][:0]u8): void { return; };");
    helpers::collect_and_check("pub const main := fn(args: [][:0]u8): i32 { return 0; };");
    helpers::collect_and_check("pub const main := fn(args: [][:0]u8): i32 { defer a = 2; };");
}

TEST_CASE("Constexpr function declaration") {
    helpers::collect_and_check("pub constexpr work := fn(): i32 { return 1; };");
}

TEST_CASE("Defer statements respect identifier collection rules") {
    helpers::test_collector_fail(
        "pub const main := fn(args: [][:0]u8): i32 { defer { var main: i32 = undefined; } };",
        sema::diagnostic{"Attempt to shadow identifier 'main'; previous declaration here: 1:11",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 56UZ}});
}

TEST_CASE("Function basic param redeclaration") {
    helpers::test_collector_fail(
        "const f := fn(f: bool): void {};",
        sema::diagnostic{"Attempt to shadow identifier 'f'; previous declaration here: 1:7",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 14UZ}});
}

TEST_CASE("Function self param redeclaration") {
    helpers::test_collector_fail(
        "const f := fn(f): void {};",
        sema::diagnostic{"Attempt to shadow identifier 'f'; previous declaration here: 1:7",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 14UZ}});
}

TEST_CASE("Function local param redeclaration") {
    helpers::test_collector_fail(
        "const f := fn(a, a: bool): void {};",
        sema::diagnostic{"Redeclaration of symbol 'a'; previous declaration here: 1:15",
                         sema::error::IDENTIFIER_REDECLARATION,
                         std::pair{0UZ, 17UZ}});
}

TEST_CASE("Function block shadowing") {
    helpers::test_collector_fail(
        "const f := fn(): void { var f := 3; };",
        sema::diagnostic{"Attempt to shadow identifier 'f'; previous declaration here: 1:7",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 28UZ}});
}

} // namespace ghoti::tests
