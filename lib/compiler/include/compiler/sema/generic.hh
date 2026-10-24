#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/sema/instance_id.hh"

#include "support/diagnostic.hh"

namespace ghoti::mod { struct module_t; } // namespace ghoti::mod

namespace ghoti::sema {

class type;

// A template when it has an `auto`/`type` parameter or a `comptime` parameter
struct generic_function_info {
    // ...and the parameter packs those functions were instantiated with, innermost last
    struct enclosing_pack {
        std::string_view                  name;
        std::vector<gsl::not_null<type*>> element_types;
    };

    gsl::not_null<mod::module_t*>            module;
    ast::node_id                             node_id;
    gsl::not_null<const ast::function_expr*> fn_expr;
    gsl::not_null<type*>                     fn_type;
    stdx::option<std::string_view>           name;
    // Enclosing struct/union/enum, if any; may not be resolved
    stdx::option<type&> enclosing_type{stdx::none};
    // For a method declared directly in an aggregate returned by a `fn(...): type` ctor
    stdx::option<usize> enclosing_fn_table_idx{stdx::none};
    // For a local generic function, the scopes of the functions around it, outermost first
    std::vector<usize>          enclosing_scopes{};
    std::vector<enclosing_pack> enclosing_packs{};
};

class generic_function_registry {
  public:
    // Where a local generic function is returned from the frame its environment points into
    struct escape {
        source_location at;
        bool            erased{false}; // returned as an erased `fn(...)`, not by its own type
        bool            reported{false};
    };

  public:
    generic_function_registry() noexcept = default;
    ~generic_function_registry()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(generic_function_registry);

    auto register_function(type&                          fn_type,
                           mod::module_t&                 module,
                           ast::node_id                   node_id,
                           const ast::function_expr&      fn_expr,
                           stdx::option<std::string_view> name                   = stdx::none,
                           stdx::option<type&>            enclosing_type         = stdx::none,
                           stdx::option<usize>            enclosing_fn_table_idx = stdx::none,
                           std::vector<usize>             enclosing_scopes       = {},
                           std::vector<generic_function_info::enclosing_pack> enclosing_packs = {})
        -> void {
        registry_.emplace(&fn_type,
                          generic_function_info{
                              .module                 = &module,
                              .node_id                = node_id,
                              .fn_expr                = &fn_expr,
                              .fn_type                = &fn_type,
                              .name                   = name,
                              .enclosing_type         = enclosing_type,
                              .enclosing_fn_table_idx = enclosing_fn_table_idx,
                              .enclosing_scopes       = std::move(enclosing_scopes),
                              .enclosing_packs        = std::move(enclosing_packs),
                          });
    }

    [[nodiscard]] auto get_opt(const type& fn_type) const noexcept
        -> stdx::option<const generic_function_info&> {
        if (auto it{registry_.find(&fn_type)}; it != registry_.end()) { return it->second; }
        return stdx::none;
    }

    auto set_function_name(type& fn_type, std::string_view name) -> void {
        if (auto it{registry_.find(&fn_type)}; it != registry_.end()) {
            it->second.name.emplace(name);
        }
    }

    auto set_type_ctor_member_prefix(const type& clone, std::string prefix) -> void {
        type_ctor_member_prefixes_.insert_or_assign(&clone, std::move(prefix));
    }

    [[nodiscard]] auto get_type_ctor_member_prefix(const type& clone) const noexcept
        -> stdx::option<std::string_view> {
        if (const auto it{type_ctor_member_prefixes_.find(&clone)};
            it != type_ctor_member_prefixes_.end()) {
            return std::string_view{it->second};
        }
        return stdx::none;
    }

    // For `mangle_arg_type` to distinguish e.g. `foo(Vec(i32))` from `foo(Vec(i64))`.
    // Keyed by instance id so a `const`/`volatile` twin of the clone mangles the same.
    auto set_clone_disc(instance_id instance, std::string disc) -> void {
        clone_discs_.insert_or_assign(instance, std::move(disc));
    }

    [[nodiscard]] auto get_clone_disc(stdx::option<instance_id> instance) const noexcept
        -> stdx::option<std::string_view> {
        if (!instance) { return stdx::none; }
        if (const auto it{clone_discs_.find(*instance)}; it != clone_discs_.end()) {
            return std::string_view{it->second};
        }
        return stdx::none;
    }

    // The environment of runtime locals a local generic function captures, shared by all of its
    // monomorphs and built at the place it's written
    auto set_capture_env(const type& fn_type, type& env) -> void {
        capture_envs_.insert_or_assign(&fn_type, &env);
    }

    [[nodiscard]] auto get_capture_env(const type& fn_type) const noexcept -> stdx::option<type&> {
        if (const auto it{capture_envs_.find(&fn_type)}; it != capture_envs_.end()) {
            return *it->second;
        }
        return stdx::none;
    }

    auto mark_escaping(const type& fn_type, escape where) -> void {
        escapes_.try_emplace(&fn_type, where);
    }

    [[nodiscard]] auto get_escape(const type& fn_type) noexcept -> stdx::option<escape&> {
        if (const auto it{escapes_.find(&fn_type)}; it != escapes_.end()) { return it->second; }
        return stdx::none;
    }

  private:
    ankerl::unordered_dense::map<gsl::not_null<const type*>, generic_function_info> registry_;
    ankerl::unordered_dense::map<gsl::not_null<const type*>, gsl::not_null<type*>>  capture_envs_;
    ankerl::unordered_dense::map<gsl::not_null<const type*>, escape>                escapes_;
    ankerl::unordered_dense::map<gsl::not_null<const type*>, std::string>
                                                           type_ctor_member_prefixes_;
    ankerl::unordered_dense::map<instance_id, std::string> clone_discs_;
};

struct generic_instantiation_request {
    gsl::not_null<type*>            generic_fn_type;
    gsl::span<gsl::not_null<type*>> arg_types;
    gsl::not_null<type*>            return_type;
    std::string                     mangled_name;
    ast::node_id                    fn_node_id;
    gsl::not_null<mod::module_t*>   module;
};

// One `const m = fn ...` member of an aggregate returned by a `fn(...): type` constructor
struct type_ctor_member_emit {
    gsl::not_null<type*> owner_clone; // the per-instantiation aggregate type
    ast::node_id         member_decl;
    std::string          gir_name;   // `<ctor-mangled>.<member>`, the emitted GIR symbol
    std::string          typing_key; // key into `context::instantiation_body_types`; may be ""
};

struct generic_instantiation_entry {
    gsl::not_null<type*> return_type;
    std::string          mangled_name;
};

} // namespace ghoti::sema
