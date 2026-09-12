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
    const bool is_explicit{triple_str && !triple_str->empty()};
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

// A stable, version-suffix-free OS token
auto normalized_target_os(const llvm::Triple& triple) -> std::string_view {
    switch (triple.getOS()) {
    case llvm::Triple::Darwin:
    case llvm::Triple::MacOSX:     return "macos";
    case llvm::Triple::IOS:        return "ios";
    case llvm::Triple::Linux:      return "linux";
    case llvm::Triple::Win32:      return "windows";
    case llvm::Triple::FreeBSD:    return "freebsd";
    case llvm::Triple::OpenBSD:    return "openbsd";
    case llvm::Triple::NetBSD:     return "netbsd";
    case llvm::Triple::DragonFly:  return "dragonfly";
    case llvm::Triple::Solaris:    return "solaris";
    case llvm::Triple::Haiku:      return "haiku";
    case llvm::Triple::WASI:       return "wasi";
    case llvm::Triple::Emscripten: return "emscripten";
    case llvm::Triple::UEFI:       return "uefi";
    case llvm::Triple::UnknownOS:  return "freestanding";
    default:                       return triple.getOSTypeName(triple.getOS());
    }
}

// Canonical LLVM arch name
auto normalized_target_arch(const llvm::Triple& triple) -> std::string_view {
    switch (triple.getArch()) {
    case llvm::Triple::x86_64:      return "x86_64";
    case llvm::Triple::x86:         return "x86";
    case llvm::Triple::aarch64:     return "aarch64";
    case llvm::Triple::arm:         return "arm";
    case llvm::Triple::thumb:       return "thumb";
    case llvm::Triple::riscv64:     return "riscv64";
    case llvm::Triple::riscv32:     return "riscv32";
    case llvm::Triple::wasm32:      return "wasm32";
    case llvm::Triple::wasm64:      return "wasm64";
    case llvm::Triple::ppc64:
    case llvm::Triple::ppc64le:     return "powerpc64";
    case llvm::Triple::ppc:         return "powerpc";
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:    return "mips64";
    case llvm::Triple::mips:
    case llvm::Triple::mipsel:      return "mips";
    case llvm::Triple::systemz:     return "s390x";
    case llvm::Triple::loongarch64: return "loongarch64";
    case llvm::Triple::sparcv9:     return "sparc64";
    default:                        return llvm::Triple::getArchTypeName(triple.getArch());
    }
}

auto normalized_target_abi(const llvm::Triple& triple) -> std::string_view {
    if (triple.getEnvironment() == llvm::Triple::UnknownEnvironment) { return "none"; }
    return triple.getEnvironmentName();
}

auto normalized_target_family(const llvm::Triple& triple) -> std::string_view {
    if (triple.isOSWindows()) { return "windows"; }
    if (triple.isWasm()) { return "wasm"; }
    switch (triple.getOS()) {
    case llvm::Triple::Darwin:
    case llvm::Triple::MacOSX:
    case llvm::Triple::IOS:
    case llvm::Triple::Linux:
    case llvm::Triple::FreeBSD:
    case llvm::Triple::OpenBSD:
    case llvm::Triple::NetBSD:
    case llvm::Triple::DragonFly:
    case llvm::Triple::Solaris:
    case llvm::Triple::Haiku:
    case llvm::Triple::AIX:       return "unix";
    default:                      return "other";
    }
}

auto normalized_target_endian(const llvm::Triple& triple) -> std::string_view {
    return triple.isLittleEndian() ? "little" : "big";
}

auto normalized_target_ptr_bits(const llvm::Triple& triple) -> u32 {
    if (triple.isArch64Bit()) { return 64U; }
    if (triple.isArch16Bit()) { return 16U; }
    return 32U;
}

auto target_facts::resolve(const llvm::Triple& triple) noexcept -> target_facts {
    return target_facts{
        .os       = normalized_target_os(triple),
        .arch     = normalized_target_arch(triple),
        .abi      = normalized_target_abi(triple),
        .family   = normalized_target_family(triple),
        .endian   = normalized_target_endian(triple),
        .ptr_bits = normalized_target_ptr_bits(triple),
    };
}

auto target_facts::resolve(stdx::option<std::string_view> triple_str) -> target_facts {
    return resolve(resolve_target_triple(triple_str));
}

auto can_emit_freestanding_entry(const llvm::Triple& triple) -> bool {
    // Every non-Linux target links a crt that provides the process entry point.
    if (!triple.isOSLinux()) { return true; }
    switch (triple.getArch()) {
    case llvm::Triple::x86_64:
    case llvm::Triple::aarch64:
    case llvm::Triple::riscv64:
    case llvm::Triple::riscv32:
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
    case llvm::Triple::loongarch64: return true;
    default:                        return false;
    }
}

auto get_default_output_extension(output_type type, stdx::option<std::string_view> triple_str)
    -> std::string_view {
    const auto triple{resolve_target_triple(triple_str)};
    switch (type) {
    case output_type::OBJECT:
        if (triple.isOSWindows() && !triple.isWindowsGNUEnvironment()) { return ".obj"; }
        return ".o";
    case output_type::EXECUTABLE:
        if (triple.isOSWindows()) { return ".exe"; }
        if (triple.isWasm()) { return ".wasm"; }
        return "";
    case output_type::STATIC_LIBRARY:
        if (triple.isOSWindows() && !triple.isWindowsGNUEnvironment()) { return ".lib"; }
        return ".a";
    case output_type::DYNAMIC_LIBRARY:
        if (triple.isOSDarwin()) { return ".dylib"; }
        if (triple.isOSWindows()) { return ".dll"; }
        return ".so";
    default: return "";
    }
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
    // One section per function/global so the linker's dead-strip can work its magic
    target_opts.FunctionSections = true;
    target_opts.DataSections     = true;
    const auto codegen_opt_level{to_llvm_codegen_opt_level(options.level)};

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

auto emit_asm_string(llvm::Module& module, llvm::TargetMachine& target_machine)
    -> stdx::result<std::string, diagnostic> {
    PROFILE_FUNCTION();

    module.setDataLayout(target_machine.createDataLayout());
    module.setTargetTriple(target_machine.getTargetTriple());

    std::string buffer;
    {
        llvm::raw_string_ostream string_os{buffer};
        llvm::buffer_ostream     dest{string_os};

        llvm::legacy::PassManager pass_manager;
        if (target_machine.addPassesToEmitFile(
                pass_manager, dest, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
            return make_codegen_err(fmt::format("Target machine '{}' cannot emit assembly",
                                                target_machine.getTargetTriple().str()),
                                    error::ASM_EMISSION_FAILED);
        }

        PROFILE_SCOPE("LLVM Assembly Emission");
        pass_manager.run(module);
    }
    return buffer;
}

} // namespace ghoti::codegen
