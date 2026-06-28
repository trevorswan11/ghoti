#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

auto collect_and_validate_label(std::string_view input, usize expected_size) -> void {
    auto [ctx, idx]{helpers::collect_and_check(input)};

    auto&       analyzer{ctx->analyzer};
    const auto& registry{analyzer.get_registry()};
    REQUIRE(registry.size() == expected_size);

    CHECK(registry.get_from_opt(idx, "a"));
    CHECK_FALSE(registry.get_from_opt(idx, "blk"));

    const auto blk_idx{idx + 1};
    const auto [sym, sym_data, type]{ctx->get_type_sym_info<sema::symbols::label>(
        "blk", blk_idx, stdx::none, &sema::symbols::label::get_definition)};
    CHECK(sym.get_kind_opt() == sema::symbol_kind::LABEL);

    CHECK(type.get_symbol_table_idx_opt() == blk_idx);
    CHECK(type == ctx->get_type(sema::type_kind::LABEL, blk_idx));
}

} // namespace

TEST_CASE("Label collection") {
    collect_and_validate_label("const a := blk: for (0..5) |i| { const foo := bar; };", 3);
    collect_and_validate_label("const a := blk: { if (b) { break :blk c; } else break :blk 5; };",
                               4);
}

TEST_CASE("Label redeclaration") {
    helpers::test_collector_fail(
        "const a := a: {};",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 12UZ}});
}

TEST_CASE("Label shadowing") {
    helpers::test_collector_fail(
        "const a := blk: { var blk: i32; };",
        sema::diagnostic{"Attempt to shadow identifier 'blk'; previous declaration here: 1:15",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 18UZ}});
}

} // namespace ghoti::tests
