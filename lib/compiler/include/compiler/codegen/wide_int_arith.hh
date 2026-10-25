#pragma once

#include <string_view>

#include <llvm/IR/Module.h>
#include <stdx/types.hh>

namespace ghoti::codegen {

// Integers up to this wide multiply and divide inline for efficiency
inline constexpr u32 MAX_INLINE_MULDIV_BITS{1'024};

// Rewrites every multiply, divide, and remainder of integers wider than `MAX_INLINE_MULDIV_BITS`
// into a call to compiler_rt's `__ghoti_*ei` routines. Run before optimization and again after, in
// case it made more.
auto expand_wide_int_arith(llvm::Module& module) -> void;

// Whether `symbol` is one of the routines `expand_wide_int_arith` calls
[[nodiscard]] auto is_wide_int_routine(std::string_view symbol) -> bool;

} // namespace ghoti::codegen
