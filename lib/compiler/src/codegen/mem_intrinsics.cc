#include "compiler/codegen/mem_intrinsics.hh"

#include <string>
#include <string_view>

#include <gsl/pointers>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Comdat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/types.hh>

namespace ghoti::codegen {

namespace {

enum class mem_kind : u8 {
    COPY,
    SET,
    MOVE,
};

[[nodiscard]] constexpr auto symbol_name(mem_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case mem_kind::COPY: return "memcpy";
    case mem_kind::SET:  return "memset";
    case mem_kind::MOVE: return "memmove";
    }
}

// Whether `kind` is reached by a live intrinsic call or an existing declaration of its symbol
[[nodiscard]] auto module_uses(const llvm::Module& module, mem_kind kind) -> bool {
    if (const auto* fn{module.getFunction(symbol_name(kind))}) { return fn->isDeclaration(); }
    for (const auto& fn : module) {
        if (fn.use_empty() || !fn.isIntrinsic()) { continue; }
        switch (fn.getIntrinsicID()) {
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memcpy_inline:
            if (kind == mem_kind::COPY) { return true; }
            break;
        case llvm::Intrinsic::memset:
        case llvm::Intrinsic::memset_inline:
            if (kind == mem_kind::SET) { return true; }
            break;
        case llvm::Intrinsic::memmove:
            if (kind == mem_kind::MOVE) { return true; }
            break;
        default: break;
        }
    }
    return false;
}

// `size_t` for the module's target
[[nodiscard]] auto size_bits(const llvm::Module& module) -> u32 {
    const llvm::Triple triple{module.getTargetTriple()};
    if (triple.isArch64Bit()) { return 64; }
    return triple.isArch16Bit() ? 16 : 32;
}

class fallback_builder {
  public:
    explicit fallback_builder(llvm::Module& module)
        : module_{module}, context_{module.getContext()}, builder_{context_},
          i8_ty_{llvm::Type::getInt8Ty(context_)}, i32_ty_{llvm::Type::getInt32Ty(context_)},
          size_ty_{llvm::Type::getIntNTy(context_, size_bits(module))},
          ptr_ty_{llvm::PointerType::getUnqual(context_)} {}

    auto define(mem_kind kind) -> void {
        auto* fn{prepare(kind)};
        if (!fn) { return; }
        auto  arg{fn->arg_begin()};
        auto* dst{arg++};
        auto* mid{arg++};
        auto* n{arg};

        auto* done{llvm::BasicBlock::Create(context_, "done", fn)};
        builder_.SetInsertPoint(done);
        builder_.CreateRet(dst);

        if (kind != mem_kind::MOVE) {
            builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", fn, done));
            byte_loop(fn, dst, mid, n, kind == mem_kind::SET, false, done);
            return;
        }

        // Overlapping ranges copy away from the overlap
        auto* fwd{llvm::BasicBlock::Create(context_, "fwd", fn, done)};
        auto* bwd{llvm::BasicBlock::Create(context_, "bwd", fn, done)};
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", fn, fwd));
        auto* di{builder_.CreatePtrToInt(dst, size_ty_)};
        auto* si{builder_.CreatePtrToInt(mid, size_ty_)};
        builder_.CreateCondBr(builder_.CreateICmpULT(di, si), fwd, bwd);
        builder_.SetInsertPoint(fwd);
        byte_loop(fn, dst, mid, n, false, false, done);
        builder_.SetInsertPoint(bwd);
        byte_loop(fn, dst, mid, n, false, true, done);
    }

  private:
    // A body-less weak `ptr (ptr, ptr|i32, i64)` to fill, or none when one is already defined
    [[nodiscard]] auto prepare(mem_kind kind) -> llvm::Function* {
        const auto name{symbol_name(kind)};
        auto*      second{kind == mem_kind::SET ? i32_ty_ : ptr_ty_};
        auto*      fn_ty{llvm::FunctionType::get(ptr_ty_, {ptr_ty_, second, size_ty_}, false)};

        auto* fn{module_.getFunction(name)};
        if (fn && !fn->isDeclaration()) { return nullptr; }
        // A declaration of another shape is a user symbol we cannot safely fill in
        if (fn && fn->getFunctionType() != fn_ty) { return nullptr; }
        if (!fn) {
            fn = llvm::Function::Create(fn_ty, llvm::GlobalValue::WeakAnyLinkage, name, module_);
        }
        fn->setLinkage(llvm::GlobalValue::WeakAnyLinkage);

        // COFF/ELF dedupe weak defs via a COMDAT; MachO folds weak symbols on its own
        if (!llvm::Triple{module_.getTargetTriple()}.isOSBinFormatMachO()) {
            fn->setComdat(module_.getOrInsertComdat(std::string{name}));
        }
        fn->addFnAttr(llvm::Attribute::NoInline);
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        fn->addFnAttr(llvm::Attribute::NoBuiltin);
        // `optnone` keeps loop-idiom recognition from turning the byte loop back into a
        // `llvm.mem*` intrinsic, which would lower to this same (recursive) call
        fn->addFnAttr(llvm::Attribute::OptimizeNone);
        return fn;
    }

    // From the current insert point: `dst[k] = <byte>` for `k` in 0..n (or n..0 when
    // `backward`), branching to `after` when done
    auto byte_loop(gsl::not_null<llvm::Function*>   fn,
                   gsl::not_null<llvm::Value*>      dst,
                   gsl::not_null<llvm::Value*>      mid,
                   gsl::not_null<llvm::Value*>      n,
                   bool                             set,
                   bool                             backward,
                   gsl::not_null<llvm::BasicBlock*> after) -> void {
        auto* zero{llvm::ConstantInt::get(size_ty_, 0)};
        auto* one{llvm::ConstantInt::get(size_ty_, 1)};
        auto* iv{builder_.CreateAlloca(size_ty_, nullptr, "i")};
        builder_.CreateStore(backward ? n.get() : static_cast<llvm::Value*>(zero), iv);
        auto* head{llvm::BasicBlock::Create(context_, "head", fn)};
        auto* body{llvm::BasicBlock::Create(context_, "body", fn)};
        builder_.CreateBr(head);

        builder_.SetInsertPoint(head);
        auto* i{builder_.CreateLoad(size_ty_, iv, "i.cur")};
        auto* cont{backward ? builder_.CreateICmpNE(i, zero) : builder_.CreateICmpULT(i, n)};
        builder_.CreateCondBr(cont, body, after);

        builder_.SetInsertPoint(body);
        auto* idx{backward ? builder_.CreateSub(i, one) : i};
        auto* dp{builder_.CreateGEP(i8_ty_, dst, idx, "dp")};
        if (set) {
            builder_.CreateStore(builder_.CreateTrunc(mid, i8_ty_), dp);
        } else {
            auto* sp{builder_.CreateGEP(i8_ty_, mid, idx, "sp")};
            builder_.CreateStore(builder_.CreateLoad(i8_ty_, sp, "b"), dp);
        }
        builder_.CreateStore(backward ? idx : builder_.CreateAdd(i, one), iv);
        builder_.CreateBr(head);
    }

  private:
    llvm::Module&      module_;
    llvm::LLVMContext& context_;
    llvm::IRBuilder<>  builder_;
    llvm::Type*        i8_ty_;
    llvm::Type*        i32_ty_;
    llvm::Type*        size_ty_;
    llvm::Type*        ptr_ty_;
};

} // namespace

auto define_mem_intrinsic_fallbacks(llvm::Module& module) -> void {
    fallback_builder builder{module};
    for (const auto kind : {mem_kind::COPY, mem_kind::SET, mem_kind::MOVE}) {
        if (module_uses(module, kind)) { builder.define(kind); }
    }
}

} // namespace ghoti::codegen
