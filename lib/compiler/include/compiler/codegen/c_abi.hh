#pragma once

#include <llvm/IR/Module.h>

namespace ghoti::codegen {

// Rewrites every signature and call that carries an aggregate by value into the form the
// target's C ABI passes it in, so any ghoti function can call or be called from C. Runs on the
// whole module so each definition and each call site agree. Targets without a model keep
// LLVM's own aggregate passing.
auto lower_c_abi(llvm::Module& module) -> void;

} // namespace ghoti::codegen
