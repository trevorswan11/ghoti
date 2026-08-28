#pragma once

#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"

namespace ghoti::sema {

// Technically a preprocessor but not really, run it before symbol collection
class cfg_pass {
  public:
    [[nodiscard]] static auto run(mod::module& module, context& ctx) -> bool;
};

} // namespace ghoti::sema
