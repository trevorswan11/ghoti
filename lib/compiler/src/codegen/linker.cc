#include "compiler/codegen/linker.hh"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <lld/Common/CommonLinkerContext.h>
#include <lld/Common/Driver.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/WindowsDriver/MSVCPaths.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/stdlib.hh"
#include "support/env.hh"
#include "support/string_utils.hh"
#include "support/subprocess.hh"

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

// A real SDK is used as the link sysroot
struct darwin_sdk {
    std::string path;
    bool        bundled{false};
};

// `SDKROOT` isn't set in every environment that has a usable macOS SDK, fall back to xcrun and
// then to the shipped stubs so cross links work from any host
[[nodiscard]] auto resolve_darwin_sdk() -> stdx::option<darwin_sdk> {
    if (const auto sdkroot{get_env("SDKROOT")}) { return darwin_sdk{std::string{*sdkroot}}; }

    static const auto cached_sdk{[] -> stdx::option<darwin_sdk> {
        piped_process proc{mock_argv{"xcrun", "-sdk", "macosx", "--show-sdk-path"}};
        const auto    exit_code{proc.close_stdin_and_wait()};
        if (exit_code && *exit_code == 0) {
            std::string path;
            std::getline(proc.stdout_stream(), path);
            string_utils::strip_trailing_cr(path);
            if (!path.empty()) { return darwin_sdk{path}; }
        }

        if (const auto bundled{mod::find_lib_path("darwin")}) {
            return darwin_sdk{bundled->string(), true};
        }
        return stdx::none;
    }()};
    return cached_sdk;
}

// The SDK version recorded in `SDKSettings.json`, which the linker stamps into the image
[[nodiscard]] auto read_darwin_sdk_version(const std::string& sdk_path)
    -> stdx::option<std::string> {
    auto buffer{llvm::MemoryBuffer::getFile(
        (std::filesystem::path{sdk_path} / "SDKSettings.json").string())};
    if (!buffer) { return stdx::none; }

    auto parsed{llvm::json::parse((*buffer)->getBuffer())};
    if (!parsed) { return stdx::none; }
    const auto* settings{parsed->getAsObject()};
    if (!settings) { return stdx::none; }
    for (const auto* key : {"Version", "MinimalDisplayName"}) {
        if (const auto version{settings->getString(key)}) { return version->str(); }
    }
    return stdx::none;
}

// Extra objects, then one `<dir_flag><dir>` per library search path
auto add_extra_inputs(std::vector<std::string>&   args,
                      const extra_linker_options& linker_opts,
                      std::string_view            dir_flag) -> void {
    for (const auto& obj : linker_opts.objects) { args.emplace_back(obj.string()); }
    for (const auto& dir : linker_opts.library_paths) {
        args.emplace_back(fmt::format("{}{}", dir_flag, dir.string()));
    }
}

// After the user's inputs so they get first claim on a symbol, but before the platform's own
// libraries
auto add_builtins(std::vector<std::string>& args, const extra_linker_options& linker_opts) -> void {
    if (linker_opts.builtins) { args.emplace_back(linker_opts.builtins->string()); }
}

auto add_unix_libraries(std::vector<std::string>& args, const extra_linker_options& linker_opts)
    -> void {
    for (const auto& lib : linker_opts.libraries) { args.emplace_back(fmt::format("-l{}", lib)); }
}

auto add_darwin_args(std::vector<std::string>&   args,
                     const llvm::Triple&         triple,
                     const std::string&          obj_path_str,
                     const std::string&          out_path_str,
                     const extra_linker_options& linker_opts,
                     bool                        is_dylib) -> void {
    args.emplace_back("ld64.lld");
    if (is_dylib) { args.emplace_back("-dylib"); }
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

    const auto sdk{resolve_darwin_sdk()};
    auto       sdk_version{min_version};
    if (sdk) {
        if (auto version{read_darwin_sdk_version(sdk->path)}) { sdk_version = std::move(*version); }
    }
    args.emplace_back(min_version);
    args.emplace_back(sdk_version);
    args.emplace_back(obj_path_str);

    add_extra_inputs(args, linker_opts, "-L");

    args.emplace_back("-o");
    args.emplace_back(out_path_str);

    args.emplace_back("-dead_strip");
    if (sdk && sdk->bundled) {
        args.emplace_back(fmt::format("-L{}", sdk->path));
    } else if (sdk) {
        args.emplace_back("-syslibroot");
        args.emplace_back(sdk->path);
    }
    add_unix_libraries(args, linker_opts);
    add_builtins(args, linker_opts);
    if (sdk) { args.emplace_back("-lSystem"); }
}

// Directories that may hold the Win32 import libraries (`kernel32.lib`, `shell32.lib`, ...).
// Populated from, in order: the `GHOTI_WIN_SYSROOT_LIB` override, the `LIB` environment
// variable, and an auto-detected Windows SDK.
[[nodiscard]] auto windows_import_lib_dirs(const llvm::Triple& triple) -> std::vector<std::string> {
    std::vector<std::string> dirs;
    const auto               add_dir{[&](std::string dir) {
        if (dir.empty()) { return; }
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) { return; }
        if (!std::ranges::contains(dirs, dir)) { dirs.emplace_back(std::move(dir)); }
    }};

    const auto add_path_list{[&](std::string_view list) {
        for (usize start{0}; start <= list.size();) {
            const auto sep{list.find(';', start)};
            const auto end{sep == std::string_view::npos ? list.size() : sep};
            if (end > start) {
                add_dir(std::string{stdx::string::substr(list, start, end - start)});
            }
            if (sep == std::string_view::npos) { break; }
            start = sep + 1;
        }
    }};

    if (const auto sysroot{get_env("GHOTI_WIN_SYSROOT_LIB")}) { add_path_list(*sysroot); }
    if (const auto lib_env{get_env("LIB")}) { add_path_list(*lib_env); }

    using sdk_path = std::pair<llvm::SmallString<128>, i32>;
    static const auto sdk{[] -> stdx::option<sdk_path> {
        auto        vfs{llvm::vfs::getRealFileSystem()};
        std::string path, include_version, lib_version;
        i32         major{0};
        if (!llvm::getWindowsSDKDir(*vfs,
                                    stdx::none,
                                    stdx::none,
                                    stdx::none,
                                    path,
                                    major,
                                    include_version,
                                    lib_version)) {
            return stdx::none;
        }
        llvm::SmallString<128> base{path};
        llvm::sys::path::append(base, "Lib");
        if (major >= 8 && !lib_version.empty()) { llvm::sys::path::append(base, lib_version); }
        return sdk_path{base, major};
    }()};

    if (sdk) {
        auto um_path{sdk->first};
        llvm::sys::path::append(um_path, "um");
        std::string arch_path;
        if (llvm::appendArchToWindowsSDKLibPath(
                sdk->second, um_path, triple.getArch(), arch_path)) {
            add_dir(std::move(arch_path));
        }
    }
    return dirs;
}

// MinGW GCC/Clang environment on Windows
auto add_mingw_args(std::vector<std::string>&   args,
                    const llvm::Triple&         triple,
                    const std::string&          obj_path_str,
                    const std::string&          out_path_str,
                    const extra_linker_options& linker_opts,
                    bool                        is_dylib) -> void {
    args.emplace_back("lld");
    if (is_dylib) { args.emplace_back("-shared"); }
    const std::string emulation{
        triple.getArch() == llvm::Triple::x86_64
            ? "i386pep"
            : (triple.getArch() == llvm::Triple::x86 ? "i386pe" : "arm64pe")};
    args.emplace_back("-m");
    args.emplace_back(emulation);
    args.emplace_back(obj_path_str);
    add_extra_inputs(args, linker_opts, "-L");
    // Auto-detected Win32 import-lib directories, so `-l` names resolve with no manual `-L`.
    for (const auto& dir : windows_import_lib_dirs(triple)) {
        args.emplace_back(fmt::format("-L{}", dir));
    }
    add_unix_libraries(args, linker_opts);
    add_builtins(args, linker_opts);
    // The entry wrapper's own Win32 API calls need their import libs listed explicitly.
    if (linker_opts.needs_windows_argv_apis) {
        args.emplace_back("-lkernel32");
        args.emplace_back("-lshell32");
    }
    args.emplace_back("--gc-sections");
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    if (is_dylib) {
        // ghoti links no CRT, so there is no `DllMainCRTStartup` to default the entry to
        args.emplace_back("--Xlink=-noentry");
    } else {
        args.emplace_back("-e");
        args.emplace_back("main");
        args.emplace_back("--subsystem");
        args.emplace_back("console");
    }
}

// MSVC environment on Windows
auto add_msvc_args(std::vector<std::string>&   args,
                   const llvm::Triple&         triple,
                   const std::string&          obj_path_str,
                   const std::string&          out_path_str,
                   const extra_linker_options& linker_opts,
                   bool                        is_dylib) -> void {
    args.emplace_back("lld-link");
    if (is_dylib) { args.emplace_back("/dll"); }
    args.emplace_back(obj_path_str);
    add_extra_inputs(args, linker_opts, "/libpath:");
    // Auto-detected Win32 import-lib directories, so `.lib` names resolve with no manual `-L`.
    for (const auto& dir : windows_import_lib_dirs(triple)) {
        args.emplace_back(fmt::format("/libpath:{}", dir));
    }
    for (const auto& lib : linker_opts.libraries) {
        args.emplace_back(lib.ends_with(".lib") ? lib : fmt::format("{}.lib", lib));
    }
    add_builtins(args, linker_opts);
    // See the equivalent comment in add_mingw_args -- these aren't supplied by a spec file here
    if (linker_opts.needs_windows_argv_apis) {
        args.emplace_back("kernel32.lib");
        args.emplace_back("shell32.lib");
    }
    args.emplace_back("/opt:ref");
    args.emplace_back("/opt:icf");
    args.emplace_back(fmt::format("/out:{}", out_path_str));
    if (is_dylib) {
        args.emplace_back("/noentry");
    } else {
        args.emplace_back("/entry:main");
        args.emplace_back("/subsystem:console");
    }
}

auto add_wasm_args(std::vector<std::string>&   args,
                   const std::string&          obj_path_str,
                   const std::string&          out_path_str,
                   const extra_linker_options& linker_opts,
                   bool                        is_dylib) -> void {
    args.emplace_back("wasm-ld");
    if (is_dylib) {
        args.emplace_back("--no-entry");
        args.emplace_back("-shared");
    }
    args.emplace_back(obj_path_str);
    add_extra_inputs(args, linker_opts, "-L");
    add_unix_libraries(args, linker_opts);
    add_builtins(args, linker_opts);
    args.emplace_back("--gc-sections");
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    if (!is_dylib) {
        args.emplace_back("-e");
        args.emplace_back("main");
    }
}

// Default to ELF for Linux and other Unix-like systems
auto add_elf_args(std::vector<std::string>&   args,
                  const llvm::Triple&         triple,
                  const std::string&          obj_path_str,
                  const std::string&          out_path_str,
                  const extra_linker_options& linker_opts,
                  bool                        is_dylib) -> void {
    args.emplace_back("ld.lld");
    if (is_dylib) { args.emplace_back("-shared"); }
    args.emplace_back(obj_path_str);
    add_extra_inputs(args, linker_opts, "-L");
    add_unix_libraries(args, linker_opts);
    add_builtins(args, linker_opts);
    args.emplace_back("--gc-sections");
    args.emplace_back("-o");
    args.emplace_back(out_path_str);
    if (!is_dylib) {
        // Linux gets a freestanding `_start` synthesized in codegen (no crt/libc)
        args.emplace_back("-e");
        args.emplace_back(triple.isOSLinux() ? "_start" : "main");
    }
}

// Names the compiler builtins among lld's undefined symbols, which lib/compiler_rt should provide
[[nodiscard]] auto missing_builtins_hint(const llvm::Triple& triple,
                                         std::string_view    error_output,
                                         bool                linked_builtins) -> std::string {
    constexpr std::string_view marker{"undefined symbol: "};
    std::vector<std::string>   missing;
    for (usize pos{error_output.find(marker)}; pos != std::string_view::npos;
         pos = error_output.find(marker, pos + marker.size())) {
        auto       rest{error_output.substr(pos + marker.size())};
        const auto end{rest.find_first_of(" \r\n")};
        auto       symbol{strip_global_prefix(triple, rest.substr(0, end))};
        if (is_compiler_rt_symbol(triple, symbol) && !std::ranges::contains(missing, symbol)) {
            missing.emplace_back(symbol);
        }
    }
    if (missing.empty()) { return {}; }
    const auto names{fmt::format("{}",
                                 fmt::join(missing | std::views::transform([](const auto& name) {
                                               return fmt::format("`{}`", name);
                                           }),
                                           ", "))};
    const bool one{missing.size() == 1};
    if (!linked_builtins) {
        return fmt::format("hint: {} {} compiler {}, and no builtins archive was linked (lib/"
                           "compiler_rt wasn't found, or --no-compiler-rt was given)",
                           names,
                           one ? "is a" : "are",
                           one ? "builtin" : "builtins");
    }
    return fmt::format("hint: {} {} compiler {}; lib/compiler_rt doesn't provide {} for '{}' yet",
                       names,
                       one ? "is a" : "are",
                       one ? "builtin" : "builtins",
                       one ? "it" : "them",
                       triple.str());
}

[[nodiscard]] auto run_link(const llvm::Triple&    triple,
                            gsl::span<std::string> arg_strings,
                            bool linked_builtins) -> stdx::result<void, diagnostic> {
    std::string              error_output;
    llvm::raw_string_ostream error_stream{error_output};
    llvm::raw_null_ostream   null_stream;
    bool                     success{false};

    {
        PROFILE_SCOPE("LLD Link Execution");
        auto args{arg_strings |
                  std::views::transform([](const auto& str) -> auto* { return str.c_str(); }) |
                  std::ranges::to<std::vector<const char*>>()};

        std::jthread linker_thread([&] {
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
        std::string message{
            fmt::format("Linking failed for target '{}':\n{}", triple.str(), error_output)};
        const auto add_hint{[&](std::string_view hint) {
            if (!message.ends_with('\n')) { message += '\n'; }
            message += hint;
        }};
        if (triple.isOSWindows() &&
            (error_output.contains("kernel32") || error_output.contains("shell32"))) {
            add_hint("hint: install the Windows SDK, build from a Developer Command Prompt (so "
                     "`LIB` is set), or set GHOTI_WIN_SYSROOT_LIB / pass -L pointing to a "
                     "directory that contains kernel32.lib");
        }
        if (const auto builtins_hint{missing_builtins_hint(triple, error_output, linked_builtins)};
            !builtins_hint.empty()) {
            add_hint(builtins_hint);
        }
        if (triple.isOSDarwin() && !resolve_darwin_sdk()) {
            add_hint("hint: no macOS SDK was found; set SDKROOT to one, or keep the "
                     "`lib/darwin` directory that ships next to ghoti");
        }
        return make_codegen_err(std::move(message), error::LINKING_FAILED);
    }
    return {};
}

[[nodiscard]] auto link_image(const std::filesystem::path& object_file,
                              const std::filesystem::path& output_file,
                              const target_options&        target_opts,
                              const extra_linker_options&  linker_opts,
                              bool is_dylib) -> stdx::result<void, diagnostic> {
    const auto               triple{resolve_target_triple(target_opts.triple_str)};
    const auto               obj_path_str{object_file.string()};
    const auto               out_path_str{output_file.string()};
    std::vector<std::string> args;

    if (triple.isOSDarwin()) {
        add_darwin_args(args, triple, obj_path_str, out_path_str, linker_opts, is_dylib);
    } else if (triple.isWindowsGNUEnvironment()) {
        add_mingw_args(args, triple, obj_path_str, out_path_str, linker_opts, is_dylib);
    } else if (triple.isOSWindows()) {
        add_msvc_args(args, triple, obj_path_str, out_path_str, linker_opts, is_dylib);
    } else if (triple.isWasm()) {
        add_wasm_args(args, obj_path_str, out_path_str, linker_opts, is_dylib);
    } else {
        add_elf_args(args, triple, obj_path_str, out_path_str, linker_opts, is_dylib);
    }
    TRY(run_link(triple, args, linker_opts.builtins.has_value()));

    // Ensure the output carries executable permissions on POSIX systems
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

} // namespace

auto reset_linker_context() -> void {
    if (lld::hasContext()) { lld::CommonLinkerContext::destroy(); }
    llvm::llvm_shutdown();
}

auto has_windows_argv_sysroot() -> bool {
    return !windows_import_lib_dirs(resolve_target_triple()).empty();
}

auto link_executable(const std::filesystem::path& object_file,
                     const std::filesystem::path& output_file,
                     const target_options&        target_opts,
                     const extra_linker_options&  linker_opts) -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    return link_image(object_file, output_file, target_opts, linker_opts, false);
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

auto link_dynamic_library(const std::filesystem::path& object_file,
                          const std::filesystem::path& output_file,
                          const target_options&        target_opts,
                          const extra_linker_options&  linker_opts)
    -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    return link_image(object_file, output_file, target_opts, linker_opts, true);
}

} // namespace ghoti::codegen
