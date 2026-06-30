#include "helpers/sema.hh"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/error.hh"
#include "helpers/common.hh"

namespace ghoti::tests::helpers {

auto test_common_decl_collection(const sema::symbol_table_registry& registry,
                                 const mod::module&                 module,
                                 usize                              idx,
                                 std::string_view                   name) -> void {
    const auto& symbol{UNWRAP(registry.get_from_opt(idx, name))};
    const auto& node{UNWRAP(symbol.get_data().as_opt<sema::symbols::node_t>())};
    CHECK_FALSE(symbol.is_public(module));
    CHECK(node.is<ast::decl_stmt>());
}

sema_test_context::sema_test_context(const std::vector<mock_file>& imports,
                                     const std::filesystem::path&  root_path,
                                     std::string_view              input,
                                     std::ostream&                 error_stream)
    : loader{stdx::make_box<mod::memory_loader>()}, manager{*loader},
      analyzer{manager, error_stream, false}, root_mod{[&] -> auto& {
          loader->add(root_path, std::string{input});
          for (const auto& mock : imports) {
              loader->add(mock.path, std::string{mock.source});
              if (mock.name) { REQUIRE(manager.add_library_module(*mock.name, mock.path)); }
          }

          return *UNWRAP(manager.try_get_file_module(root_path));
      }()} {}

auto sema_test_context::verify_registry_resolved() -> void {
    for (usize i{0}; const auto& table : analyzer.get_registry()) {
        for (const auto& [name, proxy] : table) {
            CHECK(proxy.symbol.get_status() == sema::symbol_status::RESOLVED);
            if (proxy.symbol.get_status() != sema::symbol_status::RESOLVED) {
                FAIL(name << " was not resolved in table idx " << i);
            }
        }
        i++;
    }
}

auto sema_test_context::test_common_decl_collection(usize idx, std::string_view name) -> void {
    const auto& registry{analyzer.get_registry()};
    helpers::test_common_decl_collection(registry, root_mod, idx, name);
}

auto sema_test_context::check_poisoned(const sema::symbol& sym) -> void {
    CHECK(sym.get_kind_opt() == sema::symbol_kind::POISONED);
    CHECK(sym.get_status() == sema::symbol_status::RESOLVED);
}

auto sema_test_context::check_poisoned(const sema::type& type) -> void {
    CHECK(type.is_poison());
    CHECK(type == get_type(sema::type_kind::POISON));
    CHECK(type.get_data().as_opt<sema::types::poison>());
}

auto sema_test_context::check_poisoned(const sema::symbol& sym, const sema::type& type) -> void {
    check_poisoned(sym);
    check_poisoned(type);
}

auto sema_test_context::get_string_literal_size(ast::expr_handle           handle,
                                                stdx::option<mod::module&> enclosing_mod) -> usize {
    const auto& module{enclosing_mod.value_or(root_mod)};
    const auto& str_expr{UNWRAP(module.ast.get_as_opt<ast::string_expr>(handle))};
    return str_expr.value.size() + 1;
}

auto collect(std::string_view input, const std::vector<mock_file>& imports) -> ctx_idx_pair {
    auto ctx{stdx::make_box<sema_test_context>(imports, TEST_FILENAME, input)};
    check_errors<syntax::diagnostics>(ctx->root_mod);
    ctx->analyzer.collect_symbols(ctx->root_mod);
    usize idx{UNWRAP(ctx->root_mod.root_table_idx)};
    return {std::move(ctx), idx};
}

auto collect_and_check(std::string_view input, const std::vector<mock_file>& imports)
    -> ctx_idx_pair {
    auto [ctx, idx]{collect(input, imports)};
    check_errors<sema::diagnostics>(ctx->root_mod);
    return {std::move(ctx), idx};
}

auto resolve(std::string_view input, const std::vector<mock_file>& imports) -> ctx_idx_pair {
    auto [ctx, idx]{collect(input, imports)};
    ctx->analyzer.resolve_types(ctx->root_mod);
    return {std::move(ctx), idx};
}

auto resolve_and_check(std::string_view input, const std::vector<mock_file>& imports)
    -> ctx_idx_pair {
    auto [ctx, idx]{resolve(input, imports)};
    check_errors<sema::diagnostics>(ctx->root_mod);
    ctx->verify_registry_resolved();

    return {std::move(ctx), idx};
}

} // namespace ghoti::tests::helpers
