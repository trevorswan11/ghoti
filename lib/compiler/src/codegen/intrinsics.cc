#include "compiler/codegen/intrinsics.hh"

#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/sema/type.hh"

namespace ghoti::codegen {

namespace {

[[nodiscard]] auto describe(const llvm::Type* t) -> std::string {
    std::string              out;
    llvm::raw_string_ostream os{out};
    t->print(os);
    return out;
}

// `llvm.x86.sse2.pause` -> `x86`; empty for a target-independent intrinsic
[[nodiscard]] auto target_prefix(std::string_view name) -> std::string_view {
    name.remove_prefix(std::string_view{"llvm."}.size());
    return stdx::string::substr(name, 0, name.find('.'));
}

} // namespace

auto intrinsic_value_type(llvm::LLVMContext& context, const sema::type& t, u32 ptr_bits)
    -> llvm::Type* {
    switch (t.get_kind()) {
    case sema::type_kind::INT:   return llvm::IntegerType::get(context, sema::int_width(t));
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE: return llvm::IntegerType::get(context, ptr_bits);
    case sema::type_kind::BOOL:  return llvm::Type::getInt1Ty(context);
    case sema::type_kind::F16:   return llvm::Type::getHalfTy(context);
    case sema::type_kind::F32:   return llvm::Type::getFloatTy(context);
    case sema::type_kind::F64:   return llvm::Type::getDoubleTy(context);
    case sema::type_kind::F80:   return llvm::Type::getX86_FP80Ty(context);
    case sema::type_kind::F128:  return llvm::Type::getFP128Ty(context);
    case sema::type_kind::VOID_: return llvm::Type::getVoidTy(context);
    case sema::type_kind::POINTER:
        // A thin `^T`; `^dyn I` and `^fn` are two words
        if (const auto p{t.get_data().as_opt<sema::types::pointer>()};
            p && p->underlying.get_kind() == sema::type_kind::DYN) {
            return nullptr;
        }
        if (sema::is_fat_callable(t)) { return nullptr; }
        return llvm::PointerType::getUnqual(context);
    default: return nullptr;
    }
}

auto check_llvm_intrinsic(std::string_view                   name,
                          const sema::type&                  return_type,
                          gsl::span<const sema::type* const> arg_types,
                          const llvm::Triple&                triple,
                          std::vector<usize>* immediate_args) -> stdx::option<std::string> {
    if (!name.starts_with("llvm.")) {
        return fmt::format("'{}' isn't an LLVM intrinsic name; they start with 'llvm.'", name);
    }
    const auto id{llvm::Intrinsic::lookupIntrinsicID(name)};
    if (id == llvm::Intrinsic::not_intrinsic) {
        return fmt::format("LLVM has no intrinsic named '{}'", name);
    }
    if (llvm::Intrinsic::isTargetIntrinsic(id)) {
        const auto prefix{target_prefix(name)};
        const auto arch_prefix{llvm::Triple::getArchTypePrefix(triple.getArch())};
        if (prefix != std::string_view{arch_prefix}) {
            return fmt::format("'{}' is an intrinsic for '{}', but the target is '{}'",
                               name,
                               prefix,
                               triple.getArchName().str());
        }
    }

    llvm::LLVMContext context;
    const auto        ptr_bits{triple.isArch64Bit() ? 64U : triple.isArch16Bit() ? 16U : 32U};
    const auto        lower{[&](const sema::type& t) -> llvm::Type* {
        // Without x87 hardware an `f80` is an integer, which no float intrinsic takes
        if (t.get_kind() == sema::type_kind::F80 && !triple.isX86()) { return nullptr; }
        return intrinsic_value_type(context, t, ptr_bits);
    }};

    auto* ret{lower(return_type)};
    if (!ret) { return "an LLVM intrinsic can't return this type"; }
    std::vector<llvm::Type*> params;
    for (usize i{0}; i < arg_types.size(); ++i) {
        auto* param{lower(*arg_types[i])};
        if (!param) { return fmt::format("argument {} has a type no LLVM intrinsic takes", i + 1); }
        params.emplace_back(param);
    }

    auto*                             fn_type{llvm::FunctionType::get(ret, params, false)};
    llvm::SmallVector<llvm::Type*, 4> overloads;
    std::string                       reason;
    llvm::raw_string_ostream          reason_os{reason};
    if (!llvm::Intrinsic::isSignatureValid(id, fn_type, overloads, reason_os)) {
        if (!llvm::Intrinsic::isOverloaded(id)) {
            return fmt::format("'{}' is '{}', not '{}'",
                               name,
                               describe(llvm::Intrinsic::getType(context, id)),
                               describe(fn_type));
        }
        while (!reason.empty() && (reason.back() == '\n' || reason.back() == '.')) {
            reason.pop_back();
        }
        return reason.empty()
                   ? fmt::format("'{}' has no overload '{}'", name, describe(fn_type))
                   : fmt::format("'{}' has no overload '{}': {}", name, describe(fn_type), reason);
    }

    if (immediate_args) {
        const auto attributes{llvm::Intrinsic::getAttributes(context, id, fn_type)};
        for (usize i{0}; i < params.size(); ++i) {
            if (attributes.hasParamAttr(static_cast<unsigned>(i), llvm::Attribute::ImmArg)) {
                immediate_args->emplace_back(i);
            }
        }
    }

    // A mangled name (`llvm.ctpop.i32`) must agree with the types it is called with
    if (llvm::Intrinsic::isOverloaded(id) &&
        llvm::StringRef{name} != llvm::Intrinsic::getBaseName(id)) {
        const auto mangled{llvm::Intrinsic::getNameNoUnnamedTypes(id, overloads)};
        if (llvm::StringRef{name} != mangled) {
            return fmt::format(
                "'{}' names a different overload than these types call: '{}'", name, mangled);
        }
    }
    return stdx::none;
}

} // namespace ghoti::codegen
