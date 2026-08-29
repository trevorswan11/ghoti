#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"

namespace ghoti::mod { struct module; } // namespace ghoti::mod

namespace ghoti::sema {

class type;

// A template when it has an `auto`/`type` parameter or a `constexpr` parameter
struct generic_function_info {
    gsl::not_null<mod::module*>              module;
    ast::node_id                             node_id;
    gsl::not_null<const ast::function_expr*> fn_expr;
    gsl::not_null<type*>                     fn_type;
    stdx::option<std::string_view>           name;
    // Enclosing struct/union/enum, if any; may not be resolved
    stdx::option<type&> enclosing_type{stdx::none};
};

class generic_function_registry {
  public:
    generic_function_registry() noexcept = default;
    ~generic_function_registry()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(generic_function_registry);

    auto register_function(type&                          fn_type,
                           mod::module&                   module,
                           ast::node_id                   node_id,
                           const ast::function_expr&      fn_expr,
                           stdx::option<std::string_view> name           = stdx::none,
                           stdx::option<type&>            enclosing_type = stdx::none) -> void {
        registry_.emplace(&fn_type,
                          generic_function_info{
                              .module         = &module,
                              .node_id        = node_id,
                              .fn_expr        = &fn_expr,
                              .fn_type        = &fn_type,
                              .name           = name,
                              .enclosing_type = enclosing_type,
                          });
    }

    [[nodiscard]] auto get_opt(const type& fn_type) const noexcept
        -> stdx::option<const generic_function_info&> {
        if (auto it{registry_.find(const_cast<type*>(&fn_type))}; it != registry_.end()) {
            return it->second;
        }
        return stdx::none;
    }

    auto set_function_name(type& fn_type, std::string_view name) -> void {
        if (auto it{registry_.find(&fn_type)}; it != registry_.end()) {
            it->second.name.emplace(name);
        }
    }

  private:
    ankerl::unordered_dense::map<type*, generic_function_info> registry_;
};

struct generic_instantiation_request {
    gsl::not_null<type*>        generic_fn_type;
    gsl::span<type*>            arg_types;
    gsl::not_null<type*>        return_type;
    std::string                 mangled_name;
    ast::node_id                fn_node_id;
    gsl::not_null<mod::module*> module;
};

struct generic_instantiation_entry {
    gsl::not_null<type*> return_type;
    std::string          mangled_name;
};

} // namespace ghoti::sema
