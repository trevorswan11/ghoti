#pragma once

#include <stdx/option.hh>

#include "compiler/ast/id.hh"
#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Finds the identifier_expr reference whose token spans `target`, if any
[[nodiscard]] auto identifier_at(const mod::module& module, source_location target)
    -> stdx::option<ast::node_id>;

} // namespace ghoti::lsp
