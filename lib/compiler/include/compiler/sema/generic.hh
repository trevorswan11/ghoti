#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/sema/const_arg.hh"

namespace ghoti::mod { struct module; } // namespace ghoti::mod

namespace ghoti::sema {

class type;

// A function is a template when it has an `auto` parameter *or* a non-zero mask.
struct generic_function_info {
    gsl::not_null<mod::module*>              module;
    ast::node_id                             node_id;
    gsl::not_null<const ast::function_expr*> fn_expr;
    gsl::not_null<type*>                     fn_type;
    stdx::option<std::string_view>           name;
    // Enclosing struct/union/enum, if any; may not be resolved
    stdx::option<type&> enclosing_type{stdx::none};
    u32                 constexpr_mask{0}; // Bit i set => parameter i is `constexpr`
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
                           stdx::option<type&>            enclosing_type = stdx::none,
                           u32                            constexpr_mask = 0) -> void {
        registry_.emplace(&fn_type,
                          generic_function_info{
                              .module         = &module,
                              .node_id        = node_id,
                              .fn_expr        = &fn_expr,
                              .fn_type        = &fn_type,
                              .name           = name,
                              .enclosing_type = enclosing_type,
                              .constexpr_mask = constexpr_mask,
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

struct generic_instantiation_key {
    gsl::not_null<type*> generic_fn_type;
    gsl::span<type*>     arg_types;
    // Values bound to `constexpr` parameters, in parameter order
    gsl::span<const const_arg> constexpr_args;

    [[nodiscard]] auto hash() const noexcept -> u64 {
        stdx::hasher h{reinterpret_cast<u64>(generic_fn_type.get())};
        for (const auto& arg : arg_types) {
            VERIFY(arg, "Null argument leaked from resolution");
            h.combine(reinterpret_cast<u64>(arg));
        }
        for (const auto& cx : constexpr_args) { h.combine(cx.hash()); }
        return h.finalize();
    }

    [[nodiscard]] auto operator==(const generic_instantiation_key& other) const noexcept
        -> bool = default;
};

} // namespace ghoti::sema

template <> struct ankerl::unordered_dense::hash<ghoti::sema::generic_instantiation_key> {
    using is_avalanching = void;
    using key_t          = ghoti::sema::generic_instantiation_key;
    [[nodiscard]] auto operator()(const key_t& key) const noexcept { return key.hash(); }
};

namespace ghoti::sema {

struct constexpr_binding {
    std::string_view name;
    const_arg        value;
};

struct generic_instantiation_request {
    gsl::not_null<type*>           generic_fn_type;
    gsl::span<type*>               arg_types;
    gsl::not_null<type*>           return_type;
    std::string                    mangled_name;
    ast::node_id                   fn_node_id;
    gsl::not_null<mod::module*>    module;
    std::vector<constexpr_binding> constexpr_bindings;
};

struct generic_instantiation_entry {
    gsl::not_null<type*> return_type;
    std::string          mangled_name;
};

class generic_instantiation_cache {
  public:
    generic_instantiation_cache() noexcept = default;
    ~generic_instantiation_cache()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(generic_instantiation_cache);

    [[nodiscard]] auto find(const generic_instantiation_key& key) const noexcept
        -> stdx::option<const generic_instantiation_entry&> {
        if (auto it{cache_.find(key)}; it != cache_.end()) { return it->second; }
        return stdx::none;
    }

    auto insert(generic_instantiation_key key, type& return_type, std::string mangled_name)
        -> void {
        cache_.emplace(std::move(key),
                       generic_instantiation_entry{
                           .return_type  = &return_type,
                           .mangled_name = std::move(mangled_name),
                       });
    }

  private:
    ankerl::unordered_dense::map<generic_instantiation_key, generic_instantiation_entry> cache_;
};

} // namespace ghoti::sema
