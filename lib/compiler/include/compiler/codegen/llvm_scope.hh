#pragma once

#include <stdx/utility.hh>

// Utilities for testing to prevent linking llvm/lld directly against non libcompiler libraries
namespace ghoti::codegen {

// Meant to be initialized statically to ensure llvm objects hide from stdx alloc hooks
struct llvm_global_target_init {
    llvm_global_target_init();
    ~llvm_global_target_init() = default;
    MAKE_PINNED(llvm_global_target_init);
};

// An RAII shutdown scope for testing purposes to ensure memory doesn't leak
struct llvm_scope {
    llvm_scope() = default;
    ~llvm_scope();
    MAKE_PINNED(llvm_scope);
};

} // namespace ghoti::codegen
