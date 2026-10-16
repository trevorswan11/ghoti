#pragma once

#include <vector>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace ghoti::codegen {

// Every member type of each untagged union, keyed by the union's LLVM struct, which only stores
// the one member clang would
using union_member_map = ankerl::unordered_dense::map<const llvm::Type*, std::vector<llvm::Type*>>;

// Rewrites every signature and call that carries an aggregate by value into the form the
// target's C ABI passes it in, so any ghoti function can call or be called from C. Runs on the
// whole module so each definition and each call site agree. Targets without a model keep
// LLVM's own aggregate passing.
auto lower_c_abi(llvm::Module& module, const union_member_map& unions = {}) -> void;

} // namespace ghoti::codegen
