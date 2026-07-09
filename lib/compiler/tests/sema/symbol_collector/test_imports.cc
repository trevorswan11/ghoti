#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("Import aliases correctly used") {
    auto [ctx, idx]{helpers::collect_and_check(
        R"(import foo as A; import "f.gh" as F; const foo := bar;)",
        helpers::make_vector<mock_file>(
            mock_file{.path = "foo.gh", .source = "const foo := bar;", .name = "foo"},
            mock_file{.path = "f.gh", .source = "const foo := bar;"}))};

    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == 3);
    ctx->test_common_decl_collection(idx);

    const auto test_import_inner = [&](std::string_view import_name, usize inner_idx) -> void {
        const auto [sym, sym_data, type, type_data]{
            ctx->get_full_type_sym_info<sema::symbols::node_t, sema::types::module>(import_name,
                                                                                    idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::MODULE);

        CHECK(type == ctx->get_type(sema::type_kind::MODULE, inner_idx));
        helpers::test_common_decl_collection(registry, type_data.imported, inner_idx);
    };

    test_import_inner("A", 1);
    test_import_inner("F", 2);
}

TEST_CASE("Public import query") {
    auto [ctx, idx]{
        helpers::collect_and_check("pub import std;",
                                   helpers::make_vector<mock_file>(mock_file{
                                       .path = "std.gh", .source = "var a: i32;", .name = "std"}))};

    auto&       table{UNWRAP(ctx->analyzer.get_table_opt(idx))};
    const auto& std_import{UNWRAP(table.get_opt("std"))};
    CHECK(std_import.is_public(ctx->root_mod));
}

namespace {

constexpr std::string_view a_gh{R"(pub import "b.gh" as b;)"};
constexpr std::string_view b_gh{R"(pub import "a.gh" as a;)"};

} // namespace

TEST_CASE("Circular imports") {
    auto [ctx, _]{helpers::collect_and_check(
        a_gh, helpers::make_vector<mock_file>(mock_file{.path = "b.gh", .source = b_gh}))};
    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == 2);

    CHECK(registry.get_from_opt(0, "b"));
    CHECK(registry.get_from_opt(1, "a"));
}

namespace {

constexpr std::string_view importer_gh{R"(
    import "a.gh" as a;
    import "b.gh" as b;
)"};

constexpr std::string_view diamond{R"(import std;)"};
constexpr std::string_view std_gh{R"(pub import "io.gh" as io;)"};

} // namespace

TEST_CASE("Diamond dependencies") {
    auto [ctx, _]{helpers::collect_and_check(
        importer_gh,
        helpers::make_vector<mock_file>(
            mock_file{.path = "a.gh", .source = diamond},
            mock_file{.path = "b.gh", .source = diamond},
            mock_file{.path = "std.gh", .source = std_gh, .name = "std"}))};
    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == 4);

    CHECK(registry.get_from_opt(0, "a"));
    CHECK(registry.get_from_opt(0, "b"));
    CHECK(registry.get_from_opt(1, "std"));
    CHECK(registry.get_from_opt(2, "io"));
    CHECK(registry.get_from_opt(3, "std"));
}

TEST_CASE("Self import") {
    helpers::sema_test_context ctx{{}, "self.gh", R"(import "self.gh" as self;)"};
    helpers::check_errors<syntax::diagnostics>(ctx.root_mod);
    ctx.analyzer.collect_symbols(ctx.root_mod);
    helpers::check_errors<sema::diagnostics>(ctx.root_mod);
    const auto& registry{ctx.analyzer.get_registry()};
    REQUIRE(registry.size() == 1);
    CHECK(registry.get_from_opt(0, "self"));
}

TEST_CASE("Unknown file module") {
    std::ostringstream ss;
    auto               ctx{helpers::analyze(helpers::TEST_FILENAME, ss, R"(import "a.gh" as a;)")};
    REQUIRE(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>());

    constexpr std::string_view expected{
        R"(test.gh:1:8: error: Could not find path 'a.gh' in virtual file system
    import "a.gh" as a;
           ^
)"};
    CHECK(ss.view() == expected);
}

} // namespace ghoti::tests
