#pragma once

#include <stdx/utility.hh>

// Utilities for testing to prevent linking llvm/lld directly against non libcompiler libraries
namespace ghoti::codegen {

auto llvm_init_warmup() -> void;
auto llvm_shutdown() -> void;

// An RAII shutdown scope for testing purposes to ensure memory doesn't leak
struct llvm_scope {
    llvm_scope() = default;
    ~llvm_scope();
    MAKE_PINNED(llvm_scope);
};

} // namespace ghoti::codegen
