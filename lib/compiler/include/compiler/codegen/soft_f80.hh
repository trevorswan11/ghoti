#pragma once

#include <string_view>

#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"

namespace llvm { class Triple; } // namespace llvm

namespace ghoti::codegen {

// Only x86 has x87 hardware; everywhere else an `f80` is 80 bits that compiler_rt computes on
[[nodiscard]] auto needs_soft_f80(const llvm::Triple& triple) -> bool;

// Off x86, LLVM has no routine names for `x86_fp80` arithmetic and miscompiles the rest of it, so
// this rewrites every `f80` operation into a call to the compiler_rt routine and then stores every
// `f80` as an `i80`, as Zig does. The routines take and return that `i80`, so compiler_rt can
// declare them with `f80` or `u80`. Integers wider than 128 bits go through memory:
// `__floateixf(ptr, bits)` / `__fixxfei(ptr, bits, a)` read or write the integer's in-memory
// representation. Run once, right after lowering; a no-op on x86.
[[nodiscard]] auto soften_f80(llvm::Module& module) -> stdx::result<void, diagnostic>;

// The ABI alignment `type` has once `soften_f80` stores its `f80`s as `i80`s, so a lowered layout
// is the same before and after
[[nodiscard]] auto f80_storage_align(const llvm::Module& module, llvm::Type* type) -> u64;

// Whether `symbol` is one of the routines `soften_f80` calls
[[nodiscard]] auto is_soft_f80_routine(std::string_view symbol) -> bool;

} // namespace ghoti::codegen
