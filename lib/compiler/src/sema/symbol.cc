#include "compiler/sema/symbol.hh"

#include <ranges>
#include <string_view>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

auto symbols::label::from(symbol& symbol) -> label& {
    auto label_data{symbol.get_data().as_opt<symbols::label>()};
    ASSERT(label_data, "Label is not linked to a symbolic label");
    return *label_data;
}

namespace {

template <typename Handle>
[[nodiscard]] auto span_of(const mod::module& module, Handle handle) noexcept -> source_span {
    return {module.ast.location_of(handle), module.ast.end_location_of(handle)};
}

// A decl_stmt resolves to its declared name's span, not the whole statement's
[[nodiscard]] auto symbol_span_of(const mod::module& module, const symbol::data_t& data) noexcept
    -> source_span {
    PROFILE_FUNCTION();
    return data.visit(
        [](const symbols::builtin&) -> source_span { return {}; },
        [&module](const symbols::node_t& node) -> source_span {
            if (const auto decl{module.ast.get_as_opt<ast::decl_stmt>(node)}) {
                return span_of(module, decl->name);
            }
            return span_of(module, node);
        },
        [&module](const auto& handle) -> source_span { return span_of(module, handle); },
        [&module](const symbols::label& label) -> source_span {
            return span_of(module, label.get_definition());
        },
        [&module](const symbols::struct_field& inner) -> source_span {
            return span_of(module, inner.name);
        },
        [&module](const symbols::union_field& inner) -> source_span {
            return span_of(module, inner.name);
        },
        [&module](const symbols::enumeration& inner) -> source_span {
            return span_of(module, inner.name);
        },
        [&module](const symbols::self_parameter& inner) -> source_span {
            return span_of(module, inner.name);
        },
        [&module](const symbols::parameter& inner) -> source_span {
            return span_of(module, inner.name);
        },
        [&module](const symbols::for_loop_capture& inner) -> source_span {
            return span_of(module, inner.payload);
        });
}

} // namespace

auto symbol::get_symbol_location(const mod::module& module) const noexcept -> source_location {
    return symbol_span_of(module, data_).start;
}

auto symbol::get_symbol_span(const mod::module& module) const noexcept -> source_span {
    return symbol_span_of(module, data_);
}

auto symbol::is_public(const mod::module& module) const noexcept -> bool {
    return data_.visit(
        [&module](const symbols::node_t& node) -> bool {
            switch (node->get_kind()) {
            case ast::node_kind::DECL_STATEMENT:
                return module.ast.get_as<ast::decl_stmt>(*node).has_modifier(
                    ast::decl_modifiers::PUBLIC);
            case ast::node_kind::USING_STATEMENT:
            case ast::node_kind::IMPORT_STATEMENT:
                return node->get_token_type() == syntax::token_type_t::PUBLIC;
            default: return false;
            }
        },
        [](const symbols::struct_field& field) -> bool { return field.is_public(); },
        [](const auto&) -> bool { return false; });
}

auto symbol_table::insert(std::string_view      name,
                          const mod::module&    module,
                          const symbol::data_t& data) -> stdx::result<void, diagnostic> {
    // Reserved identifier use is impossible due to a parser invariant
    PROFILE_FUNCTION();
    auto [it, inserted]{symbols_.try_emplace(name, symbol{name, data}, symbols_.size())};

    // Check for redeclaration since there's no shadowing
    if (!inserted) {
        return make_sema_err(
            fmt::format("Redeclaration of symbol '{}'; previous declaration here: {}",
                        name,
                        it->second.symbol.get_symbol_location(module)),
            error::IDENTIFIER_REDECLARATION,
            symbol_span_of(module, data).start);
    }
    return {};
}

auto symbol_table::insert_unchecked(std::string_view name, const symbol::data_t& data) -> void {
    // Reserved identifier use is impossible due to a parser invariant
    PROFILE_FUNCTION();
    auto [_, inserted]{symbols_.try_emplace(name, symbol{name, data}, symbols_.size())};
    ASSERT(inserted, "Duplicate symbol injected");
}

auto symbol_table_registry::insert_into(usize                 table_idx,
                                        const mod::module&    module,
                                        std::string_view      name,
                                        const symbol::data_t& data)
    -> stdx::result<void, diagnostic> {
    if (auto table{get_opt(table_idx)}) { return table->insert(name, module, data); }
    return make_sema_err(error::INVALID_TABLE_IDX);
}

[[nodiscard]] auto symbol_table_registry::is_shadowing(const symbol_table_stack& stack,
                                                       const mod::module&        module,
                                                       std::string_view          name,
                                                       const symbol::data_t&     data) noexcept
    -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    for (const auto idx : stack | std::views::take(stack.size() - 1)) {
        if (const auto symbol{get(idx).get_opt(name)}) {
            return make_sema_err(
                fmt::format("Attempt to shadow identifier '{}'; previous declaration here: {}",
                            name,
                            symbol_span_of(module, symbol->get_data()).start),
                error::SHADOWING_DECLARATION,
                symbol_span_of(module, data).start);
        }
    }
    return {};
}

} // namespace ghoti::sema
