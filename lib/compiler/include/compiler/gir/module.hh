#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <stdx/arena.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

struct global_decl {
    std::string         name;
    sema::type&         type;
    bool                is_constant{false};
    stdx::option<value> init_value;
    gir::linkage        linkage{linkage::INTERNAL};
    std::string         abi_name{"c"};
};

struct type_decl {
    std::string name;
    sema::type& type;
};

class module {
  public:
    explicit module(const mod::module& ast_mod, sema::arena_alloc& arena) noexcept
        : ast_module_{ast_mod}, arena_{arena} {}
    ~module() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(module);

    MAKE_GETTER(ast_module, const mod::module&);
    MAKE_DEDUCING_GETTER(types);
    MAKE_DEDUCING_GETTER(globals);
    MAKE_DEDUCING_GETTER(functions);
    MAKE_DEDUCING_GETTER(tests);
    [[nodiscard]] auto arena() noexcept -> auto& { return arena_; }

    // Resolves the raw `tests` indices into their owning functions, in declaration order.
    [[nodiscard]] auto get_test_functions() const -> std::vector<const function*> {
        std::vector<const function*> result;
        result.reserve(tests_.size());
        for (const auto idx : tests_) { result.emplace_back(functions_[idx]); }
        return result;
    }

    [[nodiscard]] auto has_function(std::string_view name) const noexcept -> bool {
        return std::ranges::any_of(functions_,
                                   [&](const auto* fn) { return fn->get_name() == name; });
    }

    auto add_type(std::string name, sema::type& type) -> type_decl&;
    auto add_global(std::string         name,
                    sema::type&         type,
                    bool                is_const,
                    stdx::option<value> init     = stdx::none,
                    gir::linkage        linkage  = linkage::INTERNAL,
                    std::string         abi_name = "c") -> global_decl&;
    auto add_function(std::string  name,
                      sema::type&  type,
                      bool         is_test      = false,
                      bool         is_constexpr = false,
                      bool         is_variadic  = false,
                      gir::linkage linkage      = linkage::INTERNAL,
                      std::string  abi_name     = "c") -> function&;

    // Names extern decls contribute to the link line (excludes the default "c" target).
    [[nodiscard]] auto get_required_libraries() const -> std::vector<std::string>;

  private:
    const mod::module&        ast_module_;
    sema::arena_alloc&        arena_;
    std::vector<type_decl*>   types_;
    std::vector<global_decl*> globals_;
    std::vector<function*>    functions_;
    std::vector<usize>        tests_;
};

} // namespace ghoti::gir
