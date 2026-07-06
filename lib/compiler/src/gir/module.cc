#include "compiler/gir/module.hh"

#include <string>
#include <utility>

#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto module::add_type(std::string name, sema::type& type) -> type_decl& {
    return *types_.emplace_back(arena_.make<type_decl>(std::move(name), type));
}

auto module::add_global(std::string name, sema::type& type, bool is_const, stdx::option<value> init)
    -> global_decl& {
    return *globals_.emplace_back(
        arena_.make<global_decl>(std::move(name), type, is_const, std::move(init)));
}

auto module::add_function(std::string name, sema::type& type, bool is_test, bool is_constexpr)
    -> function& {
    const auto idx{functions_.size()};
    functions_.emplace_back(
        arena_.make<function>(arena_, std::move(name), type, is_test, is_constexpr));
    if (is_test) { tests_.emplace_back(idx); }
    return *functions_.back();
}

} // namespace ghoti::gir
