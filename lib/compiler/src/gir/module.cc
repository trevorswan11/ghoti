#include "compiler/gir/module.hh"

#include <string>
#include <utility>

#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto module::add_type(std::string name, sema::type& type) -> type_decl& {
    types_.emplace_back(type_decl{.name = std::move(name), .type = type});
    return types_.back();
}

auto module::add_global(std::string name, sema::type& type, bool is_const, stdx::option<value> init)
    -> global_decl& {
    globals_.emplace_back(global_decl{
        .name        = std::move(name),
        .type        = type,
        .is_constant = is_const,
        .init_value  = std::move(init),
    });
    return globals_.back();
}

auto module::add_function(std::string name, sema::type& type, bool is_test, bool is_constexpr)
    -> function& {
    const auto idx{functions_.size()};
    functions_.emplace_back(function{std::move(name), type, is_test, is_constexpr});
    if (is_test) { tests_.emplace_back(idx); }
    return functions_.back();
}

} // namespace ghoti::gir
