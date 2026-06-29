#pragma once

#include <ostream>
#include <utility>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/traits.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

// A contextual wrapper around sematic steps
//
// Owns its own diagnostic list
struct context {
    mod::module_manager&         modules;
    symbol_table_registry&       registry;
    type_pool&                   pool;
    generic_function_registry&   generic_functions;
    generic_instantiation_cache& instantiation_cache;

    diagnostics    diags;
    std::ostream&  error_stream;
    stdx::opt_size prelude_index;

    context(mod::module_manager&         modules,
            symbol_table_registry&       registry,
            type_pool&                   pool,
            generic_function_registry&   generic_functions,
            generic_instantiation_cache& instantiation_cache,
            diagnostics                  diags,
            std::ostream&                error_stream) noexcept
        : modules{modules}, registry{registry}, pool{pool}, generic_functions{generic_functions},
          instantiation_cache{instantiation_cache}, diags{std::move(diags)},
          error_stream{error_stream} {}
    ~context() = default;

    // Creates a copy with identical data but a new diagnostic list
    context(const context& other)
        : modules{other.modules}, registry{other.registry}, pool{other.pool},
          generic_functions{other.generic_functions},
          instantiation_cache{other.instantiation_cache}, diags{other.diags.create_new()},
          error_stream{other.error_stream}, prelude_index{other.prelude_index} {}

    auto operator=(const context& other) -> context& = delete;
    context(context&&) noexcept                      = default;
    auto operator=(context&&) -> context&            = delete;

    // Returns false if the passed result was an error type, which is forwarded to the diagnostics
    template <typename T = void> auto try_result(stdx::result<T, diagnostic>&& result) -> bool {
        if (!result) {
            diags.emplace_back(result.error());
            return false;
        }
        return true;
    }

    // Gets the already-resolved poison type from the pool
    [[nodiscard]] auto get_poison() -> type&;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_pointer(types::mut::mutability_modifiers mutability, type& underlying)
        -> type&;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_reference(types::mut::mutability_modifiers mutability, type& underlying)
        -> type&;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_array(types::mut::mutability_modifiers mutability,
                                 bool                             null_terminated,
                                 usize                            size,
                                 type&                            underlying) -> type&;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_slice(types::mut::mutability_modifiers mutability,
                                 bool                             null_terminated,
                                 type&                            underlying) -> type&;

    // Poisons the symbol and constructs an associated diagnostic to insert into the list
    template <typename... Args> auto poison_symbol(symbol& symbol, Args&&... args) -> void {
        if constexpr (sizeof...(args) != 0) { diags.emplace_back(std::forward<Args>(args)...); }
        symbol.set_kind(symbol_kind::POISONED);
        symbol.set_status(symbol_status::RESOLVED);
    }

    // Poisons the node and constructs an associated diagnostic to insert into the list
    //
    // Returns the poison type for optional non-lookup usage
    template <ast::IndexableID ID, typename... Args>
    auto poison_node(mod::module& module, ID id, Args&&... args) -> type& {
        if constexpr (sizeof...(args) != 0) { diags.emplace_back(std::forward<Args>(args)...); }

        auto& poison{get_poison()};
        module.set_sema_type(id, poison);
        return poison;
    }

    // Creates and injects the builtin/primitive prelude and sets the internal prelude index
    auto inject_prelude() -> void;

    // Convenience function for retrieving constant builtin types
    [[nodiscard]] auto get_builtin_resolved_type(type_kind kind) -> type&;
};

} // namespace ghoti::sema
