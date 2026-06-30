#pragma once

#include <string>
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
};

struct type_decl {
    std::string name;
    sema::type& type;
};

class module {
  public:
    static constexpr usize GIR_ARENA_BLOCK_SIZE{stdx::sizes::kib(64UZ)};

  public:
    explicit module(const mod::module& ast_mod) noexcept : ast_module_{ast_mod} {}
    ~module() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(module);

    MAKE_GETTER(ast_module, const mod::module&);
    MAKE_DEDUCING_GETTER(types);
    MAKE_DEDUCING_GETTER(globals);
    MAKE_DEDUCING_GETTER(functions);
    MAKE_DEDUCING_GETTER(tests);
    [[nodiscard]] auto arena() noexcept -> auto& { return arena_; }

    auto add_type(std::string name, sema::type& type) -> type_decl&;
    auto add_global(std::string         name,
                    sema::type&         type,
                    bool                is_const,
                    stdx::option<value> init = stdx::none) -> global_decl&;
    auto add_function(std::string name,
                      sema::type& type,
                      bool        is_test      = false,
                      bool        is_constexpr = false) -> function&;

  private:
    const mod::module&                ast_module_;
    stdx::arena<GIR_ARENA_BLOCK_SIZE> arena_;
    std::vector<type_decl>            types_;
    std::vector<global_decl>          globals_;
    std::vector<function>             functions_;
    std::vector<usize>                tests_;
};

} // namespace ghoti::gir
