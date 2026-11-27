#pragma once

#include <string>
#include <vector>

#include "compiler/gir/module.hh"

namespace ghoti::gir {

// One invariant a module breaks, with the function and instruction it was found at
struct violation {
    std::string function;
    std::string message;
};

// Checks the invariants Sema guarantees and codegen relies on: well-formed control flow, locals
// defined before every use, and operands typed for their instruction
[[nodiscard]] auto verify(const module_t& mod) -> std::vector<violation>;

// Formats each violation on a line of its own
[[nodiscard]] auto format_violations(const std::vector<violation>& found) -> std::string;

} // namespace ghoti::gir
