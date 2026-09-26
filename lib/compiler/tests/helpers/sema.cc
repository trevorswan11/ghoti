#include "helpers/sema.hh"

#include <algorithm>
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

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/module.hh" // IWYU pragma: keep
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

sema_test_context::sema_test_context(const std::vector<mock_file>&  imports,
                                     const std::filesystem::path&   root_path,
                                     std::string_view               input,
                                     std::ostream&                  error_stream,
                                     stdx::option<std::string_view> target_triple)
    : loader{stdx::make_box<mod::memory_loader>()}, manager{*loader},
      analyzer{manager,
               error_stream,
               false,
               codegen::target_options{.triple_str = target_triple.transform(
                                           [](std::string_view s) { return std::string{s}; })}},
      root_mod{[&] -> auto& {
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
    return str_expr.value.size();
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

auto resolve_for_target(std::string_view input, std::string_view target_triple) -> ctx_idx_pair {
    auto ctx{stdx::make_box<sema_test_context>(
        std::vector<mock_file>{}, TEST_FILENAME, input, std::cerr, target_triple)};
    check_errors<syntax::diagnostics>(ctx->root_mod);
    ctx->analyzer.collect_symbols(ctx->root_mod);
    ctx->analyzer.resolve_types(ctx->root_mod);
    const usize idx{UNWRAP(ctx->root_mod.root_table_idx)};
    return {std::move(ctx), idx};
}

auto type_check(std::string_view input, const std::vector<mock_file>& imports) -> ctx_idx_pair {
    auto [ctx, idx]{resolve(input, imports)};
    if (!ctx->root_mod.is_poisoned()) {
        auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
        if (!ctx->root_mod.is_poisoned()) { ctx->analyzer.check_types(gir_mod, ctx->root_mod); }
    }
    return {std::move(ctx), idx};
}

auto type_check_and_verify(std::string_view input, const std::vector<mock_file>& imports)
    -> ctx_idx_pair {
    auto [ctx, idx]{type_check(input, imports)};
    check_errors<sema::diagnostics>(ctx->root_mod);
    return {std::move(ctx), idx};
}

auto expect_compile_error(std::string_view source) -> ctx_idx_pair {
    auto [ctx, idx]{helpers::type_check(source)};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    CHECK_FALSE(diags.empty());
    return {std::move(ctx), idx};
}

auto find_nested_fn(const mod::module&        module,
                    const ast::function_expr& outer,
                    std::string_view          name) -> ast::node_id {
    const auto& block{module.ast.get_as<ast::block_stmt>(outer.body)};
    for (const auto& stmt : block) {
        if (const auto decl{module.ast.get_as_opt<ast::decl_stmt>(stmt)}) {
            const auto& ident{module.ast.get_as<ast::identifier_expr>(decl->name)};
            if (ident.name == name && decl->value) { return *decl->value; }
        }
    }
    FAIL("Could not find nested function named '" << name << "'");
}

auto run_cfg(std::string_view input) -> cfg_outcome {
    auto [ctx, idx]{helpers::collect(input)};

    cfg_outcome out;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& diag : *diags) {
            out.codes.emplace_back(diag.get_error());
            out.messages.emplace_back(diag.get_message().value_or(""));
        }
    }
    return out;
}

auto selected(std::string_view input, std::string_view name) -> bool {
    auto [ctx, idx]{helpers::collect(input)};

    const auto& ast{ctx->root_mod.ast};
    // An unexpanded `@cfg` group means selection never ran
    if (std::ranges::any_of(ast, [&](ast::node_id root) { return root.is<ast::cfg_stmt>(); })) {
        return false;
    }
    return ast.find_top_level_decl(name).has_value();
}

namespace {

// The value of the top-level `name := ...` when it is a `Data` literal
template <typename Data>
auto top_level_value(const ast::AST& ast, std::string_view name) -> stdx::option<const Data&> {
    const auto decl{ast.find_top_level_decl(name)};
    if (!decl || !decl->value) { return stdx::none; }
    return ast.get_as_opt<Data>(*decl->value);
}

template <typename Range, typename Proj>
auto names_of(const ast::AST& ast, const Range& items, Proj proj) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& item : items) {
        out.emplace_back(ast.get_as<ast::identifier_expr>(proj(item)).name);
    }
    return out;
}

} // namespace

auto struct_fields(std::string_view input, std::string_view name) -> std::vector<std::string> {
    auto [ctx, idx]{helpers::collect(input)};
    const auto& ast{ctx->root_mod.ast};
    const auto  se{top_level_value<ast::struct_expr>(ast, name)};
    if (!se) { return {}; }
    return names_of(ast, se->fields, [](const auto& field) { return field.name; });
}

auto enum_variants(std::string_view input, std::string_view name) -> std::vector<std::string> {
    auto [ctx, idx]{helpers::collect(input)};
    const auto& ast{ctx->root_mod.ast};
    const auto  ee{top_level_value<ast::enum_expr>(ast, name)};
    if (!ee) { return {}; }
    return names_of(ast, ee->enumerations, [](const auto& variant) { return variant.name; });
}

auto struct_members(std::string_view input, std::string_view name) -> std::vector<std::string> {
    auto [ctx, idx]{helpers::collect(input)};
    const auto& ast{ctx->root_mod.ast};
    const auto  se{top_level_value<ast::struct_expr>(ast, name)};
    if (!se) { return {}; }
    std::vector<std::string> out;
    for (const auto& member : se->members) {
        if (const auto md{ast.get_as_opt<ast::decl_stmt>(*member)}) {
            out.emplace_back(ast.get_as<ast::identifier_expr>(md->name).name);
        }
    }
    return out;
}

auto resolver_error_codes(std::string_view src) -> std::vector<sema::error> {
    auto [ctx, idx]{helpers::resolve(src)};
    std::vector<sema::error> codes;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *diags) { codes.emplace_back(d.get_error()); }
    }
    return codes;
}

auto raised(std::string_view src, sema::error code) -> bool {
    const auto codes{resolver_error_codes(src)};
    return std::ranges::contains(codes, code);
}

auto resolve_result::message_contains(std::string_view needle) const -> bool {
    for (const auto& m : messages) {
        if (m.contains(needle)) { return true; }
    }
    return false;
}

[[nodiscard]] auto resolve_diags(std::string_view src) -> resolve_result {
    auto [ctx, idx]{helpers::resolve(src)};
    resolve_result out;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *diags) {
            out.codes.emplace_back(d.get_error());
            if (const auto& msg{d.get_message()}) { out.messages.emplace_back(*msg); }
        }
    }
    return out;
}

} // namespace ghoti::tests::helpers
