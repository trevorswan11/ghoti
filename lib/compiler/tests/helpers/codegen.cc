#include "helpers/codegen.hh"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh" // IWYU pragma: keep
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

namespace {

using namespace std::string_view_literals;

class llvm_global_target_init {
  public:
    llvm_global_target_init() {
        codegen::initialize_all_targets();

        for (const auto triple : warmup_triples) {
            for (const auto level : {codegen::opt_level::O1, codegen::opt_level::O2}) {
                codegen::target_options opts{.level = level};
                if (!triple.empty()) { opts.triple_str = std::string{triple}; }
                if (auto tm{*codegen::create_target_machine(opts)}) {
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
    }

  private:
    constexpr static std::array warmup_triples{
        ""sv,
        "x86_64-unknown-linux-gnu"sv,
        "x86_64-w64-windows-gnu"sv,
        "aarch64-unknown-linux-gnu"sv,
    };
};

// Runs llvm initialization before stdx test runner alloc hooks are initialized
llvm_global_target_init llvm_target_init_;

} // namespace

llvm_test_scope::~llvm_test_scope() { llvm::llvm_shutdown(); }

auto emit_llvm_ir(helpers::sema_test_context&       test_ctx,
               llvm::LLVMContext&                context,
               const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned", codegen::error::MODULE_LOAD_ERROR);
    }

    auto gir_mod{test_ctx.analyzer.emit_gir(test_ctx.root_mod)};
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned during GIR emission",
                                         codegen::error::MODULE_LOAD_ERROR);
    }
    return test_ctx.analyzer.emit_llvm_ir(gir_mod, context, options);
}

auto emit_object(helpers::sema_test_context&       test_ctx,
                 llvm::LLVMContext&                context,
                 const std::filesystem::path&      output_path,
                 const codegen::target_options&    target_opts,
                 const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned", codegen::error::MODULE_LOAD_ERROR);
    }

    auto gir_mod{test_ctx.analyzer.emit_gir(test_ctx.root_mod)};
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned during GIR emission",
                                         codegen::error::MODULE_LOAD_ERROR);
    }
    return test_ctx.analyzer.emit_object(gir_mod, context, target_opts, opt_options, output_path);
}

} // namespace ghoti::tests::helpers
