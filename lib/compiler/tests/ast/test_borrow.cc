#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SOURCE{"const f = fn(): void { let a = 1; let b = 2; };"};

[[nodiscard]] auto first_of(const ast::AST& tree, auto predicate) -> ast::node_id {
    const auto nodes{tree.all_nodes()};
    const auto found{std::ranges::find_if(nodes, predicate)};
    REQUIRE(found != nodes.end());
    return *found;
}

} // namespace

TEST_CASE("A borrowed tree's rewrites stay out of the tree it borrows from") {
    ghoti::arena cache_arena;
    const auto   original{helpers::parse(SOURCE, cache_arena)};
    const auto block{first_of(original, [](ast::node_id id) { return id.is<ast::block_stmt>(); })};

    ghoti::arena first_arena;
    ghoti::arena second_arena;
    auto         first{original.borrow(first_arena)};
    const auto   second{original.borrow(second_arena)};

    first.get_as_mut<ast::block_stmt>(block).statements.pop_back();
    CHECK(first.get_as<ast::block_stmt>(block).statements.size() == 1);
    CHECK(second.get_as<ast::block_stmt>(block).statements.size() == 2);
    CHECK(original.get_as<ast::block_stmt>(block).statements.size() == 2);
}

TEST_CASE("A borrowed tree copies any other node before its first write") {
    ghoti::arena cache_arena;
    const auto   original{helpers::parse(SOURCE, cache_arena)};
    const auto   decl{first_of(original, [](ast::node_id id) { return id.is<ast::decl_stmt>(); })};

    ghoti::arena arena;
    auto         borrowed{original.borrow(arena)};
    CHECK(&borrowed.get_as<ast::decl_stmt>(decl) == &original.get_as<ast::decl_stmt>(decl));

    borrowed.get_as_mut<ast::decl_stmt>(decl).is_default_wrapper = true;
    CHECK(borrowed.get_as<ast::decl_stmt>(decl).is_default_wrapper);
    CHECK_FALSE(original.get_as<ast::decl_stmt>(decl).is_default_wrapper);
}

} // namespace ghoti::tests
