#pragma once

#include <string>

#include <stdx/option.hh>

#include "compiler/ast/ast.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir {

struct dump_options {
    // Ends each instruction's line with where it was lowered from, read from `ast`
    stdx::option<const ast::AST&> locations_from;
};

[[nodiscard]] auto dump(const module_t& mod, dump_options options = {}) -> std::string;

} // namespace ghoti::uir
