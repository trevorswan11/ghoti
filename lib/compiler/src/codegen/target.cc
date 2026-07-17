#include "compiler/codegen/target.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"

namespace ghoti::codegen {

auto initialize_all_targets() noexcept -> void {
    // All of these are able to be called more than once
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
}

auto resolve_target_triple(stdx::option<std::string_view> triple_str) -> llvm::Triple {
    const auto raw_triple{triple_str
                              .and_then([](std::string_view view) -> stdx::option<std::string> {
                                  if (view.empty()) { return stdx::none; }
                                  return std::string{view};
                              })
                              .value_or(llvm::sys::getDefaultTargetTriple())};

    llvm::Triple triple{llvm::Triple::normalize(raw_triple)};
    if (triple.isOSWindows() && (triple.getEnvironment() == llvm::Triple::MSVC ||
                                 triple.getEnvironment() == llvm::Triple::UnknownEnvironment)) {
        triple.setEnvironment(llvm::Triple::GNU);
    }
    return triple;
}

auto create_target_machine(const target_options& options)
    -> stdx::result<stdx::box<llvm::TargetMachine>, diagnostic> {
    PROFILE_FUNCTION();
    initialize_all_targets();

    const auto  triple{resolve_target_triple(options.triple_str)};
    std::string error;
    const auto* target{llvm::TargetRegistry::lookupTarget(triple, error)};

    if (target == nullptr) {
        return make_codegen_err(
            fmt::format("Unable to find target for triple '{}': {}", triple.str(), error),
            error::TARGET_LOOKUP_FAILED);
    }

    llvm::TargetOptions target_opts;
    const auto          codegen_opt_level{to_llvm_codegen_opt_level(options.level)};

    auto target_machine{target->createTargetMachine(
        triple,
        options.cpu,
        options.features,
        target_opts,
        options.reloc_model ? *options.reloc_model : stdx::option<llvm::Reloc::Model>{},
        options.code_model ? *options.code_model : stdx::option<llvm::CodeModel::Model>{},
        codegen_opt_level)};

    if (!target_machine) {
        return make_codegen_err(
            fmt::format("Failed to create target machine for triple '{}'", triple.str()),
            error::TARGET_MACHINE_CREATION_FAILED);
    }

    return stdx::box<llvm::TargetMachine>{target_machine};
}

auto emit_object_file(llvm::Module&                module,
                      llvm::TargetMachine&         target_machine,
                      const std::filesystem::path& output_path) -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();

    module.setDataLayout(target_machine.createDataLayout());
    module.setTargetTriple(target_machine.getTargetTriple());

    const auto           output_path_str{output_path.string()};
    std::error_code      ec;
    llvm::raw_fd_ostream dest{output_path_str, ec, llvm::sys::fs::OF_None};

    if (ec) {
        return make_codegen_err(
            fmt::format("Could not open file '{}' for writing: {}", output_path_str, ec.message()),
            error::OBJECT_EMISSION_FAILED);
    }

    llvm::legacy::PassManager pass_manager;
    if (target_machine.addPassesToEmitFile(
            pass_manager, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return make_codegen_err(fmt::format("Target machine '{}' cannot emit an object file",
                                            target_machine.getTargetTriple().str()),
                                error::OBJECT_EMISSION_FAILED);
    }

    {
        PROFILE_SCOPE("LLVM Machine Code Emission");
        pass_manager.run(module);
        dest.flush();
    }
    return {};
}

} // namespace ghoti::codegen
