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

// The `--emit-uir` text: the imports, then each declaration's bodies, with every record printed
// by its tag's schema
[[nodiscard]] auto dump(const module& mod, dump_options options = {}) -> std::string;

} // namespace ghoti::uir
