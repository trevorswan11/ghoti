#include "compiler/codegen/llvm_scope.hh"

#include <array>
#include <string>
#include <string_view>

#include <lld/Common/CommonLinkerContext.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/enum.hh>
#include <stdx/harness/hooks.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/linker.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "support/tempfile.hh"

namespace ghoti::codegen {

auto llvm_init_warmup() -> void {
    stdx::untracked_scope untracked_guard;
    codegen::initialize_all_targets();
    using namespace std::string_view_literals;
    constexpr static std::array warmup_triples{
        ""sv,
        "x86_64-unknown-linux-gnu"sv,
        "x86_64-w64-windows-gnu"sv,
        "aarch64-unknown-linux-gnu"sv,
        "x86_64-apple-darwin"sv,
        "arm64-apple-darwin"sv,
    };

    for (const auto triple : warmup_triples) {
        for (const auto level : stdx::enum_range<codegen::opt_level>()) {
            codegen::target_options opts{.level = level};
            if (!triple.empty()) { opts.triple_str = std::string{triple}; }
            if (auto tm_res{codegen::create_target_machine(opts)}) {
                auto&             tm{*tm_res};
                llvm::LLVMContext ctx;
                llvm::Module      mod{"warmup", ctx};
                mod.setDataLayout(tm->createDataLayout());
                mod.setTargetTriple(tm->getTargetTriple());

                auto*             fn_ty{llvm::FunctionType::get(
                    llvm::Type::getInt64Ty(ctx), {llvm::Type::getInt64Ty(ctx)}, false)};
                auto*             fn{llvm::Function::Create(
                    fn_ty, llvm::Function::ExternalLinkage, "warmup_fn", mod)};
                llvm::IRBuilder<> builder{llvm::BasicBlock::Create(ctx, "entry", fn)};
                builder.CreateRet(fn->getArg(0));

                llvm::legacy::PassManager pm;
                llvm::raw_null_ostream    null_os;
                if (!tm->addPassesToEmitFile(
                        pm, null_os, nullptr, llvm::CodeGenFileType::ObjectFile)) {
                    pm.run(mod);
                }
            }
        }
    }

    {
        for (const auto triple : {""sv, "x86_64-unknown-linux-gnu"sv}) {
            for (const auto level : stdx::enum_range<codegen::opt_level>()) {
                tempfile                obj_p{"ghoti_warmup.o"};
                tempfile                exe_p{"ghoti_warmup_exe"};
                codegen::target_options opts{.level = level};
                if (!triple.empty()) { opts.triple_str = std::string{triple}; }
                if (auto tm_res{codegen::create_target_machine(opts)}) {
                    auto&             tm{*tm_res};
                    llvm::LLVMContext ctx;
                    llvm::Module      mod{"warmup_link", ctx};
                    mod.setDataLayout(tm->createDataLayout());
                    mod.setTargetTriple(tm->getTargetTriple());
                    auto*             ghoti_fn_ty{llvm::FunctionType::get(
                        llvm::Type::getVoidTy(ctx), {llvm::PointerType::getUnqual(ctx)}, false)};
                    auto*             ghoti_fn{llvm::Function::Create(
                        ghoti_fn_ty, llvm::Function::ExternalLinkage, "_ghoti_main", mod)};
                    auto*             ghoti_bb{llvm::BasicBlock::Create(ctx, "entry", ghoti_fn)};
                    llvm::IRBuilder<> gbuilder{ghoti_bb};
                    auto*             exit_fn_ty{llvm::FunctionType::get(
                        llvm::Type::getVoidTy(ctx), {llvm::Type::getInt32Ty(ctx)}, false)};
                    auto*             exit_fn{llvm::Function::Create(
                        exit_fn_ty, llvm::Function::ExternalLinkage, "exit", mod)};
                    gbuilder.CreateCall(exit_fn, {gbuilder.getInt32(0)});
                    gbuilder.CreateRetVoid();

                    auto*             fn_ty{llvm::FunctionType::get(
                        llvm::Type::getInt32Ty(ctx),
                        {llvm::Type::getInt32Ty(ctx), llvm::PointerType::getUnqual(ctx)},
                        false)};
                    auto*             fn{llvm::Function::Create(
                        fn_ty, llvm::Function::ExternalLinkage, "main", mod)};
                    llvm::IRBuilder<> builder{llvm::BasicBlock::Create(ctx, "entry", fn)};
                    builder.CreateCall(ghoti_fn, {fn->getArg(1)});
                    builder.CreateRet(builder.getInt32(0));

                    if (auto res{emit_object_file(mod, *tm, obj_p)}; res.has_value()) {
                        DISCARD(link_executable(obj_p, exe_p, opts));
                    }
                }
            }
        }
    }
}

auto llvm_shutdown() -> void {
    if (lld::hasContext()) { lld::CommonLinkerContext::destroy(); }
    llvm::llvm_shutdown();
}

llvm_scope::~llvm_scope() {
    if (lld::hasContext()) { lld::CommonLinkerContext::destroy(); }
}

} // namespace ghoti::codegen
