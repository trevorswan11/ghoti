#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <gsl/pointers>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto test_loop(std::string_view input, usize expected_reg_count, usize loop_block_idx)
    -> stdx::box<helpers::sema_test_context> {
    auto [ctx, idx]{helpers::collect_and_check(input)};

    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == expected_reg_count);
    const auto [sym, sym_data, node_data]{
        ctx->get_ast_sym_info<sema::symbols::node_t, ast::decl_stmt>("a", 0)};
    CHECK_FALSE(sym.has_kind());

    const auto& actual_type{helpers::unwrap(ctx->root_mod.get_sema_type_opt(*node_data.value))};
    CHECK(actual_type == ctx->get_type(sema::type_kind::BLOCK, loop_block_idx));
    return std::move(ctx);
}

} // namespace

TEST_CASE("Do-while loop collection") {
    auto ctx{
        test_loop("const a := do { const foo := bar; } while (blk: { const foo := bar; });", 4, 1)};
    ctx->test_common_decl_collection(1);
    ctx->test_common_decl_collection(3);
}

TEST_CASE("For loop collection") {
    auto ctx{test_loop("const a := for (0..5, blk: { const foo := bar; }) |i, j| { const foo := "
                       "bar; } else { const foo := bar; };",
                       5,
                       3)};

    const auto& loop_table{ctx->analyzer.get_table(3)};
    const auto& i_symbol{helpers::unwrap(loop_table.get_opt("i"))};
    CHECK(i_symbol.get_data().as_opt<sema::symbols::for_loop_capture>());
    const auto& j_symbol{helpers::unwrap(loop_table.get_opt("i"))};
    CHECK(j_symbol.get_data().as_opt<sema::symbols::for_loop_capture>());

    ctx->test_common_decl_collection(2);
    ctx->test_common_decl_collection(3);
    ctx->test_common_decl_collection(4);
}

TEST_CASE("Infinite loop collection") {
    auto ctx{test_loop("const a := loop { const foo := bar; };", 2, 1)};
    ctx->test_common_decl_collection(1);
}

TEST_CASE("While loop collection") {
    auto ctx{test_loop("const a := while (blk: { const foo := bar; }) : (i += blk: { const foo "
                       ":= bar; }) { const foo := bar; } else { const foo := bar; };",
                       7,
                       5)};
    ctx->test_common_decl_collection(2);
    ctx->test_common_decl_collection(4);
    ctx->test_common_decl_collection(5);
    ctx->test_common_decl_collection(6);
}

TEST_CASE("Well-placed loop control flow") {
    SECTION("For loops") {
        helpers::collect_and_check("const a := for (0..5) |i| { break; };");
        helpers::collect_and_check("const a := for (0..5) |i| { continue; };");
    }

    SECTION("Do-while loop") {
        helpers::collect_and_check("const a := do { break; } while (true);");
        helpers::collect_and_check("const a := do { continue; } while (true);");
    }

    SECTION("Infinite loop") {
        helpers::collect_and_check("const a := loop { break; };");
        helpers::collect_and_check("const a := loop { continue; };");
    }

    SECTION("While loops") {
        helpers::collect_and_check("const a := while (true) { break; };");
        helpers::collect_and_check("const a := while (true) { continue; };");
    }
}

TEST_CASE("Non-break collected as separate scope") {
    helpers::collect_and_check(
        "const a := for (0..5) |i| { const foo := bar; } else { const foo := bar; };");

    helpers::collect_and_check(
        "const a := while (true) : (i += 1) { const foo := bar; } else { const foo := bar; };");
}

TEST_CASE("Non-break collection shadowing") {
    helpers::test_collector_fail(
        "const a := for (0..5) |i| { const foo := bar; } else { var a: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 55UZ}});

    helpers::test_collector_fail(
        "const a := while (true) : (i += 1) { const foo := bar; } else { var a: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 64UZ}});
}

TEST_CASE("Shadowing in loops") {
    helpers::test_collector_fail(
        "const a := for (0..5) |i| { var a: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 28UZ}});

    helpers::test_collector_fail(
        "const a := for (0..5) |i| { var i: i32; };",
        sema::diagnostic{"Redeclaration of symbol 'i'; previous declaration here: 1:24",
                         sema::error::IDENTIFIER_REDECLARATION,
                         std::pair{0UZ, 28UZ}});

    helpers::test_collector_fail(
        "const a := loop { var a: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 18UZ}});

    helpers::test_collector_fail(
        "const a := while (true) { var a: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 26UZ}});
}

} // namespace ghoti::tests
