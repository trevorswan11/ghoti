#pragma once

#include <vector>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"

namespace ghoti::codegen {

// Every member type of each untagged union, keyed by the union's LLVM struct, which only stores
// the one member clang would
using union_member_map = ankerl::unordered_dense::map<const llvm::Type*, std::vector<llvm::Type*>>;

// A struct the type translator padded with `[N x i8]` elements for `@[align(n)]`: the element
// index of each real field, and the alignment LLVM's own layout understates
struct padded_layout {
    std::vector<u32> field_indices;
    u64              alignment;
};

using padded_struct_map = ankerl::unordered_dense::map<const llvm::Type*, padded_layout>;

[[nodiscard]] auto to_llvm_callconv(ast::calling_convention conv,
                                    const llvm::Triple& triple) noexcept -> llvm::CallingConv::ID;

// Whether `lower_c_abi` knows how C passes aggregates for `conv` on `triple`
[[nodiscard]] auto has_c_abi_model(const llvm::Triple&   triple,
                                   llvm::CallingConv::ID conv = llvm::CallingConv::C) -> bool;

// Rewrites every signature and call that carries an aggregate by value into the form the
// target's C ABI passes it in, so any ghoti function can call or be called from C. Runs on the
// whole module so each definition and each call site agree. Targets without a model keep
// LLVM's own aggregate passing, which sema keeps away from C boundaries.
auto lower_c_abi(llvm::Module&            module,
                 const union_member_map&  unions = {},
                 const padded_struct_map& padded = {}) -> void;

} // namespace ghoti::codegen
