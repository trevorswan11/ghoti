#include "compiler/codegen/target.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"

namespace ghoti::codegen {

auto to_llvm_codegen_opt_level(opt_level level) noexcept -> llvm::CodeGenOptLevel {
    switch (level) {
    case opt_level::O0: return llvm::CodeGenOptLevel::None;
    case opt_level::O1: return llvm::CodeGenOptLevel::Less;
    case opt_level::O2: return llvm::CodeGenOptLevel::Default;
    case opt_level::O3: return llvm::CodeGenOptLevel::Aggressive;
    case opt_level::Os: return llvm::CodeGenOptLevel::Default;
    case opt_level::Oz: return llvm::CodeGenOptLevel::Default;
    default:            return llvm::CodeGenOptLevel::None;
    }
}

namespace {

auto to_llvm_reloc_model(reloc_model model) noexcept -> llvm::Reloc::Model {
    switch (model) {
    case reloc_model::STATIC:         return llvm::Reloc::Static;
    case reloc_model::PIC_:           return llvm::Reloc::PIC_;
    case reloc_model::DYNAMIC_NO_PIC: return llvm::Reloc::DynamicNoPIC;
    case reloc_model::ROPI:           return llvm::Reloc::ROPI;
    case reloc_model::RWPI:           return llvm::Reloc::RWPI;
    case reloc_model::ROPI_RWPI:      return llvm::Reloc::ROPI_RWPI;
    }
}

auto to_llvm_code_model(code_model model) noexcept -> llvm::CodeModel::Model {
    switch (model) {
    case code_model::TINY:   return llvm::CodeModel::Tiny;
    case code_model::SMALL:  return llvm::CodeModel::Small;
    case code_model::KERNEL: return llvm::CodeModel::Kernel;
    case code_model::MEDIUM: return llvm::CodeModel::Medium;
    case code_model::LARGE:  return llvm::CodeModel::Large;
    }
}

} // namespace

auto initialize_all_targets() noexcept -> void {
    // All of these are able to be called more than once
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
}

auto resolve_target_triple(stdx::option<std::string_view> triple_str) -> llvm::Triple {
    const bool is_explicit{triple_str.has_value() && !triple_str->empty()};
    const auto raw_triple{is_explicit ? std::string{*triple_str}
                                      : llvm::sys::getDefaultTargetTriple()};

    llvm::Triple triple{llvm::Triple::normalize(raw_triple)};
    if (triple.isOSWindows()) {
        const bool explicit_msvc{is_explicit &&
                                 llvm::StringRef{*triple_str}.contains_insensitive("msvc")};
        if (!explicit_msvc && (triple.getEnvironment() == llvm::Triple::MSVC ||
                               triple.getEnvironment() == llvm::Triple::UnknownEnvironment)) {
            triple.setEnvironment(llvm::Triple::GNU);
        }
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

    stdx::option<llvm::Reloc::Model> reloc;
    if (options.reloc) { reloc.emplace(to_llvm_reloc_model(*options.reloc)); }

    stdx::option<llvm::CodeModel::Model> code;
    if (options.code) { code.emplace(to_llvm_code_model(*options.code)); }

    auto target_machine{target->createTargetMachine(
        triple, options.cpu, options.features, target_opts, reloc, code, codegen_opt_level)};

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
