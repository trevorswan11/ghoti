#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/sema/type.hh"

namespace llvm {

class LLVMContext;
class Triple;
class Type;

} // namespace llvm

namespace ghoti::codegen {

// The LLVM type a value of `t` has when passed to or returned from an intrinsic, or `nullptr` for
// a type an intrinsic can't take
[[nodiscard]] auto
intrinsic_value_type(llvm::LLVMContext& context, const sema::type& t, u32 ptr_bits) -> llvm::Type*;

// Why `@llvmIntrinsic(name, ret, args...)` can't call the intrinsic on `triple`, or `none` when it
// can: the name must be an intrinsic, a target intrinsic must be the target's, and the types must
// fit its signature. When it can, `immediate_args` gets the arguments that must be constants
[[nodiscard]] auto check_llvm_intrinsic(std::string_view                   name,
                                        const sema::type&                  return_type,
                                        gsl::span<const sema::type* const> arg_types,
                                        const llvm::Triple&                triple,
                                        std::vector<usize>*                immediate_args = nullptr)
    -> stdx::option<std::string>;

} // namespace ghoti::codegen
