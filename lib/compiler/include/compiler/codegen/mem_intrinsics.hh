#pragma once

#include <llvm/IR/Module.h>

namespace ghoti::codegen {

// ghoti links no C runtime, yet LLVM lowers `llvm.mem{cpy,set,move}` (and calls the optimizer
// forms from plain loops) to the C symbols `memcpy`/`memset`/`memmove`. This defines each one the
// module uses as a weak byte loop, turning an existing declaration into that definition. Safe to
// run more than once, and after optimization.
auto define_mem_intrinsic_fallbacks(llvm::Module& module) -> void;

} // namespace ghoti::codegen
