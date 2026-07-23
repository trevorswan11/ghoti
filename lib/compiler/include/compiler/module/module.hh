#pragma once

#include <concepts>
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/traits.hh"
#include "compiler/module/error.hh"
#include "compiler/module/source_loader.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/syntax/error.hh"
#include "support/source_file.hh"

namespace ghoti::mod {

enum class module_state : u8 {
    PARSED,
    SYMBOLS_COLLECTED,
    TYPE_RESOLVING,
    TYPE_RESOLVED,
    TYPE_CHECKED,

    POISONED_SYMBOL_COLLECTION,
    POISONED_TYPE_RESOLVING,
    POISONED_TYPE_RESOLVED,
    POISONED_TYPE_CHECKED,
    ERRORED,
};

using diagnotic_list_variant =
    stdx::variant<stdx::monostate, syntax::diagnostics, sema::diagnostics>;

struct module {
    std::filesystem::path                            path;
    std::filesystem::path                            parent_path;
    source_file                                      source;
    ast::AST                                         ast;
    sema::side_tables                                sema_side_tables;
    stdx::opt_size                                   root_table_idx;
    module_state                                     state{module_state::PARSED};
    std::vector<sema::generic_instantiation_request> generic_instantiations;

    diagnotic_list_variant diagnostics{stdx::monostate{}};

    module(std::filesystem::path path,
           std::filesystem::path parent_path,
           source_file           source) noexcept :path{std::move(path)},
        parent_path{std::move(parent_path)}, source{std::move(source)} {}

    ~module() = default;
    MAKE_MOVE_ONLY(module)

    // Errors out the module regardless of previous state and emplaces the diagnostics
    template <typename DiagList>
        requires(!std::same_as<std::remove_cvref_t<DiagList>, stdx::monostate>)
    auto error_out(DiagList&& list, module_state error_state) noexcept -> mod::module_state {
        state = error_state;
        diagnostics.emplace<DiagList>(std::forward<DiagList>(list));
        return state;
    }

    // Prints the modules diagnostics to the stream, doing nothing if an error state is not present
    auto print_diagnostics(std::ostream& os) const -> void;

    // Errored modules cannot be used in any future compilation step
    [[nodiscard]] auto is_errored() const noexcept -> bool {
        return state == module_state::ERRORED;
    }

    // Poisoned modules are able to be used in sematic steps but are not correct themselves
    [[nodiscard]] auto is_poisoned() const noexcept -> bool {
        return state >= module_state::POISONED_SYMBOL_COLLECTION && !is_errored();
    }

    // Indicates if the module is neither errored nor poisoned
    [[nodiscard]] auto is_ok() const noexcept -> bool {
        return state < module_state::POISONED_SYMBOL_COLLECTION;
    }

    [[nodiscard]] auto is_collectable() const noexcept -> bool {
        return state == mod::module_state::PARSED && !root_table_idx;
    }

    // Checks if symbol collection has run, allowing poisoned states
    [[nodiscard]] auto is_resolvable() const noexcept -> bool {
        return root_table_idx && (state == mod::module_state::SYMBOLS_COLLECTED ||
                                  state == mod::module_state::POISONED_SYMBOL_COLLECTION);
    }

    template <ast::IndexableID ID>
    [[nodiscard]] constexpr auto has_sema_type(ID id) const noexcept -> bool {
        if constexpr (ast::IndexableNodeID<ID>) {
            return sema_side_tables.node_types[id].has_value();
        } else {
            return sema_side_tables.explicit_types[id].has_value();
        }
    }

    [[nodiscard]] auto has_sema_type(const ast::match_expr::arm& arm) const noexcept -> bool {
        return sema_side_tables.match_arm_types[arm.pattern].has_value();
    }

    template <ast::IndexableID ID>
    [[nodiscard]] constexpr auto get_sema_type_opt(this auto&& self, ID id) noexcept {
        if constexpr (ast::IndexableNodeID<ID>) {
            return self.sema_side_tables.node_types[id];
        } else {
            return self.sema_side_tables.explicit_types[id];
        }
    }

    template <ast::IndexableID ID>
    [[nodiscard]] constexpr auto get_sema_type(this auto&& self, ID id) noexcept -> auto& {
        return *self.get_sema_type_opt(id);
    }

    template <ast::IndexableID ID>
    [[nodiscard]] auto get_generic_call_target_opt(ID id) const noexcept
        -> stdx::option<std::string_view> {
        if constexpr (ast::IndexableNodeID<ID>) {
            if (const auto& target{sema_side_tables.generic_call_targets[id]}) {
                return std::string_view{*target};
            }
        }
        return stdx::none;
    }

    template <ast::IndexableID ID> auto set_generic_call_target(ID id, std::string target) -> void {
        if constexpr (ast::IndexableNodeID<ID>) {
            sema_side_tables.generic_call_targets[id].emplace(std::move(target));
        }
    }

    [[nodiscard]] auto get_sema_type_opt(this auto&&                 self,
                                         const ast::match_expr::arm& arm) noexcept {
        return self.sema_side_tables.match_arm_types[arm.pattern];
    }

    [[nodiscard]] auto get_sema_type(const ast::match_expr::arm& arm) const noexcept -> auto& {
        return *get_sema_type_opt(arm);
    }

    template <ast::IndexableID ID>
    constexpr auto set_sema_type(ID id, sema::type& type) noexcept -> void {
        if constexpr (ast::IndexableNodeID<ID>) {
            sema_side_tables.node_types[id].emplace(type);
        } else {
            sema_side_tables.explicit_types[id].emplace(type);
        }
    }

    // Sets the sema type only if there wasn't one already, returns true if modified
    template <ast::IndexableID ID>
    constexpr auto set_sema_type_if(ID id, sema::type& type) noexcept -> bool {
        if (has_sema_type(id)) { return false; }
        if constexpr (ast::IndexableNodeID<ID>) {
            sema_side_tables.node_types[id].emplace(type);
        } else {
            sema_side_tables.explicit_types[id].emplace(type);
        }
        return true;
    }

    auto set_sema_type(const ast::match_expr::arm& arm, sema::type& type) noexcept -> void {
        sema_side_tables.match_arm_types[arm.pattern].emplace(type);
    }
};

#undef MAKE_MODULE_DIAGNOSTIC_UNPACKER

class module_manager {
  public:
    explicit module_manager(source_loader& loader) noexcept : loader_{loader} {}
    ~module_manager() = default;

    MAKE_MOVE_CONSTRUCTABLE_ONLY(module_manager)

    // Attempts to load the path from the loader and parse its contents.
    //
    // Asserts that the path is relative and its parent is absolute
    [[nodiscard]] auto try_get_file_module(const std::filesystem::path& path,
                                           const std::filesystem::path& parent_path = {})
        -> stdx::result<gsl::not_null<module*>, diagnostic>;

    // Attempts to load the module from the loader and parse its contents
    [[nodiscard]] auto try_get_library_module(std::string_view name)
        -> stdx::result<gsl::not_null<module*>, diagnostic>;

    // Adds a library module and its underlying path to the lookup table
    [[nodiscard]] auto add_library_module(std::string_view name, const std::filesystem::path& path)
        -> stdx::result<void, diagnostic>;

  private:
    [[nodiscard]] auto try_get(const std::filesystem::path& path)
        -> stdx::result<gsl::not_null<module*>, diagnostic>;

  private:
    source_loader&                                                         loader_;
    ankerl::unordered_dense::map<std::filesystem::path, stdx::box<module>> modules_;

    // Maps physical ghoti modules to their path on disk
    ankerl::unordered_dense::
        map<std::string, std::filesystem::path, stdx::string_transparent_hash, std::equal_to<>>
            module_lut_;
};

} // namespace ghoti::mod
