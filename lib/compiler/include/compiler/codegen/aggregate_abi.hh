#pragma once

#include <llvm/IR/Module.h>

namespace ghoti::codegen {

// Instruction selection expands a first-class aggregate into one value per element, so a large
// one can never be passed, returned, loaded, or stored directly. This rewrites every signature
// carrying one to pass it by pointer (a returned one through an `sret` pointer), then turns each
// whole-aggregate copy into a `memcpy`. Returns whether any `memcpy` was emitted.
auto lower_large_aggregates(llvm::Module& module) -> bool;

} // namespace ghoti::codegen
