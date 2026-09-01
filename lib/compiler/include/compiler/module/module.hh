#pragma once

#include <concepts>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/iterator.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/traits.hh"
#include "compiler/module/error.hh"
#include "compiler/module/source_loader.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/syntax/error.hh"
#include "support/diagnostic.hh"
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

struct cfg_value_result {
    bool         boolean{false};
    bool         is_predicate{true};
    ast::node_id chosen{ast::node_id::make_invalid()};
};

// Which arm of an `if constexpr` the resolver folded to
enum class if_branch : u8 {
    CONSEQUENCE,
    ALTERNATE,
};

struct module {
    std::filesystem::path                            path;
    std::filesystem::path                            parent_path;
    source_file                                      source;
    ast::AST                                         ast;
    sema::side_tables                                sema_side_tables;
    stdx::opt_size                                   root_table_idx;
    module_state                                     state{module_state::PARSED};
    std::vector<sema::generic_instantiation_request> generic_instantiations;

    // Member functions of `fn(...): type` constructor results defined in this module
    std::vector<sema::type_ctor_member_emit> type_ctor_member_emits;

    // `@cfgValue` node index -> the cfg pass's evaluated verdict
    ankerl::unordered_dense::map<usize, cfg_value_result> cfg_value_results;

    // `if constexpr` node index -> the arm the type resolver folded to
    ankerl::unordered_dense::map<usize, if_branch> if_constexpr_results;

    // `match` (on a compile-time `type`) node index -> the arm index the resolver selected
    ankerl::unordered_dense::map<usize, usize> match_arm_results;

    // Every identifier_expr references and uses encountered during symbol collection/resolution
    std::vector<ast::node_id> identifier_positions;

    // Every `import` node in this module, at any nesting depth
    std::vector<ast::node_id> import_nodes;

    diagnotic_list_variant diagnostics{stdx::monostate{}};

    // A permanent copy of the syntax diagnostics found at parse time, if any
    std::vector<diagnostic_snapshot> parse_diagnostics;

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

    template <ast::IndexableID ID>
    [[nodiscard]] auto get_resolved_symbol_owner_opt(ID id) const noexcept -> stdx::opt_size {
        if constexpr (ast::IndexableNodeID<ID>) {
            return sema_side_tables.resolved_symbol_owners[id];
        }
        return {};
    }

    template <ast::IndexableID ID> auto set_resolved_symbol_owner(ID id, usize owner_idx) -> void {
        if constexpr (ast::IndexableNodeID<ID>) {
            sema_side_tables.resolved_symbol_owners[id].emplace(owner_idx);
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

    // Records that `function_id` implicitly captures `name` merging with any existing entry
    auto add_capture(ast::node_id function_id, std::string_view name, sema::capture_usage usage)
        -> void {
        auto& captures{sema_side_tables.function_captures[function_id]};
        for (auto& capture : captures) {
            if (capture.name == name) {
                if (usage == sema::capture_usage::MUTATED) { capture.usage = usage; }
                return;
            }
        }
        captures.emplace_back(sema::capture_info{name, usage});
    }

    [[nodiscard]] auto get_captures(ast::node_id function_id) const noexcept
        -> gsl::span<const sema::capture_info> {
        return sema_side_tables.function_captures[function_id];
    }

    // Records that `id` is an identifier_expr worth indexing by position, for the LSP
    auto add_identifier_position(ast::node_id id) -> void { identifier_positions.emplace_back(id); }

    // Records where the identifier at `id` resolved to, for LSP go-to-definition
    template <ast::IndexableID ID>
    auto set_identifier_definition(ID id, located_span definition) -> void {
        if constexpr (ast::IndexableNodeID<ID>) {
            sema_side_tables.identifier_definitions[id].emplace(std::move(definition));
        } else {
            sema_side_tables.explicit_type_definitions[id].emplace(std::move(definition));
        }
    }

    template <ast::IndexableID ID>
    [[nodiscard]] auto get_identifier_definition(ID id) const -> stdx::option<located_span> {
        if constexpr (ast::IndexableNodeID<ID>) {
            return sema_side_tables.identifier_definitions[id];
        } else {
            return sema_side_tables.explicit_type_definitions[id];
        }
    }
};

class module_manager {
  public:
    using module_name_map = ankerl::unordered_dense::map<std::string,
                                                         std::filesystem::path,
                                                         stdx::string_transparent_hash,
                                                         stdx::string_transparent_eq>;
    using module_table    = ankerl::unordered_dense::map<std::filesystem::path, stdx::box<module>>;
    MAKE_ITERATOR(modules, module_table, modules_)

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

    // Prints every poisoned/errored module's diagnostics
    auto               print_all_diagnostics(std::ostream& os) const -> void;
    [[nodiscard]] auto get_or_create_builtin_module(std::string_view source) -> module&;

    [[nodiscard]] auto builtin_module() -> module& {
        ASSERT(builtin_module_, "the `builtin` module has not been created yet");
        return *builtin_module_;
    }

    [[nodiscard]] auto has_builtin_module() const noexcept -> bool {
        return static_cast<bool>(builtin_module_);
    }

  private:
    [[nodiscard]] auto try_get(const std::filesystem::path& path)
        -> stdx::result<gsl::not_null<module*>, diagnostic>;

  private:
    source_loader&    loader_;
    module_table      modules_;
    stdx::box<module> builtin_module_;

    // Maps physical ghoti modules to their path on disk
    module_name_map module_lut_;
};

} // namespace ghoti::mod
