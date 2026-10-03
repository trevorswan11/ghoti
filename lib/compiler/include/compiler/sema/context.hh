#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/arena.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/traits.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"

namespace ghoti::ast { struct block_stmt; } // namespace ghoti::ast

namespace ghoti::sema {

/// Tracks the active lexical block and statement position currently being type-resolved
using comptime_frame = ankerl::unordered_dense::map<std::string_view, gir::const_value>;

struct active_block_frame {
    stdx::option<const ast::block_stmt&> block{};
    usize                                current_stmt_idx{0};
    stdx::option<bool>                   runtime_safety{}; // set by `@setRuntimeSafety`
    usize                                safety_fn_depth{0};
    // A `for comptime` body's `comptime let mut` values as the previous iteration left them
    stdx::option<comptime_frame> carried{};
};

// Mirrors `builtin.OptimizeMode`
enum class optimize_mode : u8 {
    DEBUG,
    RELEASE_SAFE,
    RELEASE_FAST,
    RELEASE_SMALL,
};

[[nodiscard]] constexpr auto optimize_mode_name(optimize_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case optimize_mode::DEBUG:         return "debug";
    case optimize_mode::RELEASE_SAFE:  return "release_safe";
    case optimize_mode::RELEASE_FAST:  return "release_fast";
    case optimize_mode::RELEASE_SMALL: return "release_small";
    }
    return "debug";
}

// What a use of a `@[deprecated]` name reports
enum class deprecation_policy : u8 {
    WARN,
    DENY,
    ALLOW,
};

// One symbol `@export` defines for a function declaration
struct function_export {
    stdx::option<const mod::module&> owner;
    ast::node_id                     fn_node; // the exported function literal
    std::string                      name;
    bool                             weak{false};
    ast::symbol_visibility           visibility{ast::symbol_visibility::DEFAULT};
    source_location                  site;
};

// Every `@export` in the program, shared by every module's context
class export_registry {
  public:
    auto add(function_export entry) -> void { exports_.emplace_back(std::move(entry)); }

    // The exports of one function literal, in the order they were written
    [[nodiscard]] auto of(const mod::module& owner, ast::node_id fn_node) const
        -> std::vector<const function_export*> {
        std::vector<const function_export*> found;
        for (const auto& entry : exports_) {
            if (entry.owner && entry.owner == owner &&
                entry.fn_node.get_index() == fn_node.get_index() &&
                entry.fn_node.get_kind() == fn_node.get_kind()) {
                found.emplace_back(&entry);
            }
        }
        return found;
    }

    [[nodiscard]] auto named(std::string_view name) const -> stdx::option<const function_export&> {
        for (const auto& entry : exports_) {
            if (entry.name == name) { return entry; }
        }
        return stdx::none;
    }

  private:
    std::vector<function_export> exports_;
};

// A contextual wrapper around sematic steps
//
// Owns its own diagnostic list
struct context {
    mod::module_manager&         modules;
    symbol_table_registry&       registry;
    type_pool&                   pool;
    generic_function_registry&   generic_functions;
    generic_instantiation_cache& instantiation_cache;
    impl_registry&               impls;
    ghoti::arena&                arena;

    diagnostics             diags;
    std::ostream&           error_stream;
    stdx::opt_size          prelude_index;
    codegen::target_options target_opts;
    std::string             user_main_name{"main"};
    bool                    runtime_safety{true};
    optimize_mode           build_mode{optimize_mode::DEBUG};
    deprecation_policy      deprecated_policy{deprecation_policy::WARN};

    // `path:line:column` of every deprecated use already reported, so re-resolution stays quiet
    ankerl::unordered_dense::set<std::string> reported_deprecations;

    // For the generic instantiation currently being resolved or emitted
    std::vector<comptime_frame> comptime_binding_frames;

    // Nonzero while resolving a `comptime { ... }` body, whose folds are compile-time evaluation
    usize comptime_evaluation_depth{0};

    // Declared names for user struct/enum/union types, for `@typeName`
    type_name_map& user_type_names;

    // `@export`ed symbols, shared with every imported module's context
    export_registry& exports;

    // Cache of read embedded files (path string -> optional file contents)
    ankerl::unordered_dense::map<std::string, stdx::option<std::string>> embed_cache;

    // Global epoch counter tracking type environment mutations
    // Observed by all const_eval memo caches.
    u64 env_epoch{0};

    // Persists across all const evaluators
    usize eval_unroll_limit{256};

    // Nesting of in-progress generic instantiations, bounded so runaway recursion is reported
    usize                  generic_instantiation_depth{0};
    static constexpr usize max_generic_instantiation_depth{256};
    // Monomorphs whose body is being resolved, by mangled name, so a recursive call reuses them
    ankerl::unordered_dense::map<std::string, type*> instantiations_in_progress;
    // Module constants whose initializers are being folded, so one reading itself is caught
    ankerl::unordered_dense::set<const void*> globals_in_evaluation;

    auto advance_epoch() noexcept -> u64 { return ++env_epoch; }

    context(mod::module_manager&         modules,
            symbol_table_registry&       registry,
            type_pool&                   pool,
            generic_function_registry&   generic_functions,
            generic_instantiation_cache& instantiation_cache,
            impl_registry&               impls,
            ghoti::arena&                arena,
            diagnostics                  diags,
            std::ostream&                error_stream,
            codegen::target_options      target_opts = {}) noexcept
        : modules{modules}, registry{registry}, pool{pool}, generic_functions{generic_functions},
          instantiation_cache{instantiation_cache}, impls{impls}, arena{arena},
          diags{std::move(diags)}, error_stream{error_stream}, target_opts{std::move(target_opts)},
          user_type_names{*arena.make<type_name_map>()}, exports{*arena.make<export_registry>()} {}
    ~context() = default;

    // Creates a copy with identical data but a new diagnostic list
    context(const context& other)
        : modules{other.modules}, registry{other.registry}, pool{other.pool},
          generic_functions{other.generic_functions},
          instantiation_cache{other.instantiation_cache}, impls{other.impls}, arena{other.arena},
          diags{other.diags.create_new()}, error_stream{other.error_stream},
          prelude_index{other.prelude_index}, target_opts{other.target_opts},
          user_main_name{other.user_main_name}, runtime_safety{other.runtime_safety},
          build_mode{other.build_mode}, deprecated_policy{other.deprecated_policy},
          comptime_binding_frames{other.comptime_binding_frames},
          comptime_evaluation_depth{other.comptime_evaluation_depth},
          user_type_names{other.user_type_names}, exports{other.exports},
          embed_cache{other.embed_cache}, env_epoch{other.env_epoch} {}

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

    // A character literal's untyped constant: `comptime_int`, but `u21` once it needs a type
    [[nodiscard]] auto get_char_constant() -> type&;
    // The concrete type an untyped `t` takes when nothing asks for one: `i32`, `u21` for a
    // character constant, or `f64`; any other type is itself
    [[nodiscard]] auto default_concrete(type& t) -> type&;

    // Pools and resolves the arbitrary-width integer type `iN` / `uN`
    [[nodiscard]] auto get_int(u16                              bits,
                               bool                             is_signed,
                               types::mut::mutability_modifiers mutability = types::mut::CONSTANT)
        -> type&;

    // The integer `@backingInt`/`@fromBackingInt` convert through: an enum's underlying type, a
    // bit-packed struct/union's `uN`, or a tagged union's `i32` tag. None for any other type.
    [[nodiscard]] auto backing_int_type(const type& t, u32 ptr_bits) -> stdx::option<type&>;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_pointer(types::mut::mutability_modifiers mutability, type& underlying)
        -> type&;

    // Calls resolve_if on the resulting type
    [[nodiscard]] auto get_reference(types::mut::mutability_modifiers mutability, type& underlying)
        -> type&;

    // A structural (selfless) function type as spelled by `fn(...): R` / `extern fn(...): R`
    [[nodiscard]] auto get_function(gsl::span<type*>        params,
                                    type&                   return_type,
                                    bool                    is_variadic,
                                    ast::calling_convention conv,
                                    bool                    erased) -> type&;

    // The same signature as `fn`, toggled between erased and thin
    [[nodiscard]] auto with_erasure(type& fn, bool erased) -> type&;

    // `@Fn`'s result: `get_function` for a plain signature, else a thin method-shaped type
    [[nodiscard]] auto get_function_like(gsl::span<type*>        params,
                                         type&                   return_type,
                                         bool                    has_self,
                                         bool                    is_variadic,
                                         ast::calling_convention conv,
                                         bool                    erased) -> type&;

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

    // A compiler-known target-fact enum by name, from the prelude
    [[nodiscard]] auto get_builtin_type(std::string_view name) -> type&;

    [[nodiscard]] auto type_display_name(const type& t) const -> std::string;
    // "Type mismatch in store: cannot assign 'from' to 'to'", with the rejection reason if any
    [[nodiscard]] auto store_mismatch_message(const type& from, const type& to, u32 ptr_bits) const
        -> std::string;

    // The bound value of a `comptime` parameter named `name`, searching innermost frame first
    [[nodiscard]] auto lookup_comptime_binding(std::string_view name) const
        -> stdx::option<const gir::const_value&>;

    // Reads embedded file into embed_cache and returns reference to contents if successful
    [[nodiscard]] auto read_embed_file(const std::filesystem::path& path)
        -> stdx::option<const std::string&>;
};

// Marks everything folded while it lives as compile-time evaluation (a `comptime` block, label,
// or declaration initializer), as opposed to an opportunistic fold of runtime code
class comptime_evaluation_scope {
  public:
    comptime_evaluation_scope(context& ctx, bool enabled) noexcept
        : depth_{ctx.comptime_evaluation_depth}, enabled_{enabled} {
        if (enabled_) { ++depth_; }
    }
    ~comptime_evaluation_scope() noexcept {
        if (enabled_) { --depth_; }
    }
    MAKE_PINNED(comptime_evaluation_scope);

  private:
    usize& depth_;
    bool   enabled_;
};

} // namespace ghoti::sema
