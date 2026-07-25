#include "compiler/sema/analyzer.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <gsl/util>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <stdx/memory.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/codegen/llvm_optimizer.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/symbol_collector.hh"
#include "compiler/sema/passes/type_checker.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/error.hh"

namespace ghoti::sema {

auto analyzer::analyze(const std::filesystem::path& entry_path) -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    auto module_result{modules_.try_get_file_module(entry_path)};
    if (!module_result) {
        return make_sema_err(std::move(module_result.error()).get_message(),
                             error::MODULE_LOAD_ERROR);
    }

    auto module{*module_result};
    if (module->diagnostics.is<syntax::diagnostics>()) { module->print_diagnostics(error_stream_); }

    collect_symbols(*module);
    resolve_types(*module);

    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    auto gir_mod{emit_gir(*module)};
    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    check_types(gir_mod, *module);
    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    return {};
}

auto analyzer::collect_symbols(mod::module& module) -> mod::module_state {
    return symbol_collector::collect_symbols(module, ctx_);
}

auto analyzer::resolve_types(mod::module& module) -> mod::module_state {
    return type_resolver::resolve_types(module, ctx_);
}

auto analyzer::emit_gir(mod::module& module) -> gir::module {
    gir::emitter emitter{ctx_, module};
    return emitter.emit();
}

auto analyzer::check_types(gir::module& gir_module, mod::module& ast_module) -> mod::module_state {
    return type_checker::check_types(gir_module, ast_module, ctx_);
}

auto analyzer::emit_llvm_ir(gir::module&                      gir_module,
                            llvm::LLVMContext&                context,
                            const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    codegen::llvm_lowering lowering{context, gir_module.get_ast_module().path.string()};
    auto                   llvm_mod{lowering.lower(gir_module)};

    if (options.target_machine) {
        llvm_mod->setDataLayout(options.target_machine->createDataLayout());
        llvm_mod->setTargetTriple(options.target_machine->getTargetTriple());
    }

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    if (llvm::verifyModule(*llvm_mod, &os)) {
        return codegen::make_codegen_err(err_str, codegen::error::VERIFICATION_FAILED);
    }

    if (options.level != codegen::opt_level::O0 || options.debug_logging || options.time_passes) {
        codegen::llvm_optimizer optimizer{llvm_mod->getContext()};
        TRY(optimizer.optimize(*llvm_mod, options));
    }

    return llvm_mod;
}

auto analyzer::emit_object(gir::module&                      gir_module,
                           const codegen::target_options&    target_opts,
                           const codegen::optimizer_options& opt_options,
                           const std::filesystem::path&      output_path)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_object(gir_module, context, target_opts, opt_options, output_path);
}

auto analyzer::emit_object(gir::module&                      gir_module,
                           llvm::LLVMContext&                context,
                           const codegen::target_options&    target_opts,
                           const codegen::optimizer_options& opt_options,
                           const std::filesystem::path&      output_path)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    auto target_machine{TRY(codegen::create_target_machine(target_opts))};

    codegen::optimizer_options opts{opt_options};
    opts.target_machine = target_machine.get();
    if (opts.level == codegen::opt_level::O0 && target_opts.level != codegen::opt_level::O0) {
        opts.level = target_opts.level;
    }

    auto llvm_mod{TRY(emit_llvm_ir(gir_module, context, opts))};
    return codegen::emit_object_file(*llvm_mod, *target_machine, output_path);
}

auto analyzer::validate_main_entry(const mod::module& root_module) const
    -> stdx::result<void, diagnostic> {
    if (!root_module.root_table_idx) {
        return make_sema_err(
            "missing required 'main' function with signature 'fn(args: [][:0]u8): void'",
            error::TYPE_MISMATCH);
    }

    const auto& root_table{registry_.get(*root_module.root_table_idx)};
    const auto  main_sym_opt{root_table.get_opt("main")};
    if (!main_sym_opt) {
        return make_sema_err(
            "missing required 'main' function with signature 'fn(args: [][:0]u8): void'",
            error::TYPE_MISMATCH);
    }

    const auto& main_sym{*main_sym_opt};
    if (!main_sym.is_public(root_module)) {
        return make_sema_err("'main' function must be declared 'pub'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    if (main_sym.get_kind_opt() != symbol_kind::CALLABLE) {
        return make_sema_err("'main' must be a callable function",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto node_sym{main_sym.get_data().as_opt<symbols::node_t>()};
    if (!node_sym) {
        return make_sema_err("'main' must be a user-defined function",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto sema_type_opt{root_module.get_sema_type_opt(*node_sym)};
    if (!sema_type_opt) {
        return make_sema_err("failed to resolve type for 'main'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto& sema_type{*sema_type_opt};
    if (sema_type.get_kind() != type_kind::FUNCTION) {
        return make_sema_err("'main' must have function type",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto& fn_data{sema_type.get_data().as<types::function>()};
    if (fn_data.params.size() != 1) {
        return make_sema_err(
            fmt::format(
                "'main' function must have exactly 1 parameter of type '[][:0]u8', found {}",
                fn_data.params.size()),
            error::TYPE_MISMATCH,
            main_sym.get_symbol_location(root_module));
    }

    const auto* param_type{fn_data.params.front()};
    bool        valid_param{false};
    if (param_type && param_type->get_kind() == type_kind::SLICE) {
        const auto outer_slice{param_type->get_data().as_opt<types::slice>()};
        if (outer_slice && !outer_slice->null_terminated &&
            outer_slice->underlying.get_kind() == type_kind::SLICE) {
            const auto inner_slice{outer_slice->underlying.get_data().as_opt<types::slice>()};
            if (inner_slice->null_terminated &&
                inner_slice->underlying.get_kind() == type_kind::U8) {
                valid_param = true;
            }
        }
    }

    if (!valid_param) {
        return make_sema_err("'main' parameter must have type '[][:0]u8'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    if (fn_data.return_type.get_kind() != type_kind::VOID) {
        return make_sema_err("'main' return type must be 'void'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    return {};
}

auto analyzer::emit_llvm_ir_executable(gir::module&                      gir_module,
                                       llvm::LLVMContext&                context,
                                       const codegen::optimizer_options& options,
                                       std::string_view                  user_main_name)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    codegen::llvm_lowering lowering{context, gir_module.get_ast_module().path.string()};
    auto                   llvm_mod{lowering.lower_executable(gir_module, user_main_name)};

    if (options.target_machine) {
        llvm_mod->setDataLayout(options.target_machine->createDataLayout());
        llvm_mod->setTargetTriple(options.target_machine->getTargetTriple());
    }

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    if (llvm::verifyModule(*llvm_mod, &os)) {
        return codegen::make_codegen_err(err_str, codegen::error::VERIFICATION_FAILED);
    }

    if (options.level != codegen::opt_level::O0 || options.debug_logging || options.time_passes) {
        codegen::llvm_optimizer optimizer{llvm_mod->getContext()};
        TRY(optimizer.optimize(*llvm_mod, options));
    }

    return llvm_mod;
}

auto analyzer::emit_executable(gir::module&                         gir_module,
                               const codegen::target_options&       target_opts,
                               const codegen::optimizer_options&    opt_options,
                               const std::filesystem::path&         output_path,
                               const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_executable(gir_module, context, target_opts, opt_options, output_path, linker_opts);
}

auto analyzer::emit_executable(gir::module&                         gir_module,
                               llvm::LLVMContext&                   context,
                               const codegen::target_options&       target_opts,
                               const codegen::optimizer_options&    opt_options,
                               const std::filesystem::path&         output_path,
                               const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    auto target_machine{TRY(codegen::create_target_machine(target_opts))};

    codegen::optimizer_options opts{opt_options};
    opts.target_machine = target_machine.get();
    if (opts.level == codegen::opt_level::O0 && target_opts.level != codegen::opt_level::O0) {
        opts.level = target_opts.level;
    }

    auto llvm_mod{TRY(emit_llvm_ir_executable(gir_module, context, opts))};

    auto temp_obj_path{output_path};
    temp_obj_path.replace_extension(".tmp.o");
    const auto cleanup{gsl::finally([&] {
        std::error_code ec;
        std::filesystem::remove(temp_obj_path, ec);
    })};

    TRY(codegen::emit_object_file(*llvm_mod, *target_machine, temp_obj_path));
    return codegen::link_executable(temp_obj_path, output_path, target_opts, linker_opts);
}

} // namespace ghoti::sema
