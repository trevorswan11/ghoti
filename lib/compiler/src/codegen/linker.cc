#include "compiler/codegen/linker.hh"

#include <filesystem>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <lld/Common/CommonLinkerContext.h>
#include <lld/Common/Driver.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/target.hh"
#include "support/env.hh"

LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(wasm)

namespace ghoti::codegen {

namespace {

constexpr auto exe_permissions{
    std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
    std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
    std::filesystem::perms::others_exec};

auto add_darwin_args(std::vector<std::string>&   args,
                     const llvm::Triple&         triple,
                     const std::string&          obj_path_str,
                     const std::string&          out_path_str,
                     const extra_linker_options& linker_opts) -> void {
    args.emplace_back("ld64.lld");
    args.emplace_back("-arch");
    const std::string arch_name{
        triple.getArch() == llvm::Triple::aarch64 ? "arm64" : std::string{triple.getArchName()}};
    args.emplace_back(arch_name);
    args.emplace_back("-platform_version");
    args.emplace_back("macos");

    std::string min_version;
    const auto  os_ver{triple.getOSVersion()};
    if (os_ver.getMajor() != 0) {
        min_version = os_ver.getAsString();
    } else if (const auto env_ver{get_env("MACOSX_DEPLOYMENT_TARGET")}) {
        min_version = *env_ver;
    } else if (triple.getArch() == llvm::Triple::aarch64) {
        min_version = "11.0.0";
    } else {
        min_version = "10.15.0";
    }

    std::string sdk_version = min_version;
    args.emplace_back(min_version);
    args.emplace_back(sdk_version);
    args.emplace_back(obj_path_str);

    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("-L{}", dir.string()));
    }

    args.emplace_back("-o");
    args.emplace_back(out_path_str);

    if (const auto sdkroot{get_env("SDKROOT")}) {
        args.emplace_back("-syslibroot");
        args.emplace_back(*sdkroot);
    }
    args.emplace_back("-lSystem");
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(fmt::format("-l{}", lib)); }
}

// MinGW GCC/Clang environment on Windows
auto add_mingw_args(std::vector<std::string>&   args,
                    const llvm::Triple&         triple,
                    const std::string&          obj_path_str,
                    const std::string&          out_path_str,
                    const extra_linker_options& linker_opts) -> void {
    args.emplace_back("lld-link");
    args.emplace_back("-lldmingw");
    const std::string emulation{
        triple.getArch() == llvm::Triple::x86_64
            ? "i386pep"
            : (triple.getArch() == llvm::Triple::x86 ? "i386pe" : "arm64pe")};
    args.emplace_back("-m");
    args.emplace_back(emulation);
    args.emplace_back(obj_path_str);
    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("-L{}", dir.string()));
    }
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(fmt::format("-l{}", lib)); }
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    args.emplace_back("-e");
    args.emplace_back("main");
}

// MSVC environment on Windows
auto add_msvc_args(std::vector<std::string>&   args,
                   const std::string&          obj_path_str,
                   const std::string&          out_path_str,
                   const extra_linker_options& linker_opts) -> void {
    args.emplace_back("lld-link");
    args.emplace_back(obj_path_str);
    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("/libpath:{}", dir.string()));
    }
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(lib); }
    args.emplace_back(fmt::format("/out:{}", out_path_str));
    args.emplace_back("/entry:main");
    args.emplace_back("/subsystem:console");
}

auto add_wasm_args(std::vector<std::string>&   args,
                   const std::string&          obj_path_str,
                   const std::string&          out_path_str,
                   const extra_linker_options& linker_opts) -> void {
    args.emplace_back("wasm-ld");
    args.emplace_back(obj_path_str);
    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("-L{}", dir.string()));
    }
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(fmt::format("-l{}", lib)); }
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    args.emplace_back("-e");
    args.emplace_back("main");
}

// Default to ELF for Linux and other Unix-like systems
auto add_elf_args(std::vector<std::string>&   args,
                  const std::string&          obj_path_str,
                  const std::string&          out_path_str,
                  const extra_linker_options& linker_opts) -> void {
    args.emplace_back("ld.lld");
    args.emplace_back(obj_path_str);
    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("-L{}", dir.string()));
    }
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(fmt::format("-l{}", lib)); }
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    args.emplace_back("-e");
    args.emplace_back("main");
}

} // namespace

auto reset_linker_context() -> void {
    if (lld::hasContext()) { lld::CommonLinkerContext::destroy(); }
    llvm::llvm_shutdown();
}

auto link_executable(const std::filesystem::path& object_file,
                     const std::filesystem::path& output_file,
                     const target_options&        target_opts,
                     const extra_linker_options&  linker_opts) -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();

    const auto triple{resolve_target_triple(target_opts.triple_str)};
    const auto obj_path_str{object_file.string()};
    const auto out_path_str{output_file.string()};

    std::vector<std::string> arg_strings;

    if (triple.isOSDarwin()) {
        add_darwin_args(arg_strings, triple, obj_path_str, out_path_str, linker_opts);
    } else if (triple.isWindowsGNUEnvironment()) {
        add_mingw_args(arg_strings, triple, obj_path_str, out_path_str, linker_opts);
    } else if (triple.isOSWindows()) {
        add_msvc_args(arg_strings, obj_path_str, out_path_str, linker_opts);
    } else if (triple.isWasm()) {
        add_wasm_args(arg_strings, obj_path_str, out_path_str, linker_opts);
    } else {
        add_elf_args(arg_strings, obj_path_str, out_path_str, linker_opts);
    }

    std::string              error_output;
    llvm::raw_string_ostream error_stream{error_output};
    llvm::raw_null_ostream   null_stream;
    bool                     success{false};

    {
        PROFILE_SCOPE("LLD Link Execution");
        std::jthread linker_thread([&] {
            auto args{arg_strings |
                      std::views::transform([](const auto& str) -> auto* { return str.c_str(); }) |
                      std::ranges::to<std::vector<const char*>>()};

            if (triple.isOSDarwin()) {
                success = lld::macho::link(args, null_stream, error_stream, false, false);
            } else if (triple.isWindowsGNUEnvironment()) {
                success = lld::mingw::link(args, null_stream, error_stream, false, false);
            } else if (triple.isOSWindows()) {
                success = lld::coff::link(args, null_stream, error_stream, false, false);
            } else if (triple.isWasm()) {
                success = lld::wasm::link(args, null_stream, error_stream, false, false);
            } else {
                success = lld::elf::link(args, null_stream, error_stream, false, false);
            }
            if (lld::hasContext()) { lld::CommonLinkerContext::destroy(); }
        });
    }

    if (!success) {
        return make_codegen_err(
            fmt::format("Linking failed for target '{}':\n{}", triple.str(), error_output),
            error::LINKING_FAILED);
    }

    // Ensure output file has executable permissions on POSIX systems
    std::error_code ec;
    std::filesystem::permissions(
        output_file, exe_permissions, std::filesystem::perm_options::add, ec);

    if (ec) {
        return make_codegen_err(
            fmt::format("Failed to edit executable permissions:\n{}", ec.message()),
            error::PERMISSIONS_ERROR);
    }
    return {};
}

auto create_static_library(const std::filesystem::path&           output_file,
                           gsl::span<const std::filesystem::path> object_files,
                           const target_options& target_opts) -> stdx::result<void, diagnostic> {
    const auto triple{resolve_target_triple(target_opts.triple_str)};
    const auto kind{llvm::object::Archive::getDefaultKindForTriple(triple)};

    std::vector<llvm::NewArchiveMember> members;
    members.reserve(object_files.size());
    for (const auto& obj_path : object_files) {
        auto member{llvm::NewArchiveMember::getFile(obj_path.string(), false)};
        if (!member) {
            return make_codegen_err(fmt::format("Failed to read object file '{}' for archiving: {}",
                                                obj_path.string(),
                                                llvm::toString(member.takeError())),
                                    error::OBJECT_READ_FAILED);
        }
        members.emplace_back(std::move(*member));
    }

    // Make parent directories to prevent creation error
    if (output_file.has_parent_path()) {
        std::error_code ec;

        std::filesystem::create_directories(output_file.parent_path(), ec);
        if (ec) {
            return make_codegen_err(fmt::format("Failed to create parent directories for file '{}'",
                                                output_file.string()),
                                    error::DIRECTORY_CREATION_FAILED);
        }
    }

    // Write out the archive
    auto write_err{llvm::writeArchive(
        output_file.string(), members, llvm::SymtabWritingMode::NormalSymtab, kind, true, false)};
    if (write_err) {
        return make_codegen_err(fmt::format("Failed to create static archive '{}': {}",
                                            output_file.string(),
                                            llvm::toString(std::move(write_err))),
                                error::ARCHIVING_FAILED);
    }
    return {};
}

} // namespace ghoti::codegen
