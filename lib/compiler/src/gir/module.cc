#include "compiler/gir/module.hh"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto module::add_type(std::string name, sema::type& type) -> type_decl& {
    return *types_.emplace_back(arena_.make<type_decl>(std::move(name), type));
}

auto module::add_global(std::string         name,
                        sema::type&         type,
                        bool                is_const,
                        stdx::option<value> init,
                        gir::linkage        linkage,
                        std::string         abi_name) -> global_decl& {
    return *globals_.emplace_back(arena_.make<global_decl>(
        std::move(name), type, is_const, std::move(init), linkage, std::move(abi_name)));
}

auto module::add_function(std::string  name,
                          sema::type&  type,
                          bool         is_test,
                          bool         is_constexpr,
                          bool         is_variadic,
                          gir::linkage linkage,
                          std::string  abi_name) -> function& {
    const auto idx{functions_.size()};
    functions_.emplace_back(arena_.make<function>(arena_,
                                                  std::move(name),
                                                  type,
                                                  is_test,
                                                  is_constexpr,
                                                  is_variadic,
                                                  linkage,
                                                  std::move(abi_name)));
    if (is_test) { tests_.emplace_back(idx); }
    return *functions_.back();
}

auto module::get_required_libraries() const -> std::vector<std::string> {
    std::vector<std::string> libraries;
    const auto add_if_required{[&](gir::linkage linkage, const std::string& abi_name) {
        if (linkage != gir::linkage::EXTERN || abi_name == "c") { return; }
        if (!std::ranges::contains(libraries, abi_name)) { libraries.emplace_back(abi_name); }
    }};

    for (const auto* fn : functions_) { add_if_required(fn->get_linkage(), fn->get_abi_name()); }
    for (const auto* global : globals_) { add_if_required(global->linkage, global->abi_name); }
    return libraries;
}

} // namespace ghoti::gir
