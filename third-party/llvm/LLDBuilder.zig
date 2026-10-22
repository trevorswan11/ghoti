//! Clang Source Compilation rules. All artifacts are compiled as ReleaseSafe.
const std = @import("std");

const LLVMBuilder = @import("LLVMBuilder.zig");

const stdx = LLVMBuilder.stdx;
const coff = @import("sources/lld/coff.zig");
const common = @import("sources/lld/common.zig");
const elf = @import("sources/lld/elf.zig");
const macho = @import("sources/lld/macho.zig");
const mingw = @import("sources/lld/mingw.zig");
const wasm = @import("sources/lld/wasm.zig");

const Artifact = LLVMBuilder.Artifact;
const ArtifactCreateConfig = LLVMBuilder.ArtifactCreateConfig;
const ArtifactWithGen = LLVMBuilder.ArtifactWithGen;

const Metadata = struct {
    root: std.Build.LazyPath,
    lld_include: std.Build.LazyPath,
    vcs_version: *std.Build.Step.ConfigHeader,
};

/// Artifacts compiled for the actual target, associated with the lld subproject of llvm
const LLDTargetArtifacts = struct {
    const Common = struct {
        core_lib: Artifact = undefined,
        version_inc: *std.Build.Step.ConfigHeader = undefined,
    };

    coff: ArtifactWithGen = .{},
    common: Common = .{},
    elf: ArtifactWithGen = .{},
    macho: ArtifactWithGen = .{},
    mingw: ArtifactWithGen = .{},
    wasm: ArtifactWithGen = .{},
};

const Self = @This();

b: *std.Build,
llvm: *const LLVMBuilder,
metadata: Metadata,

lld_artifacts: LLDTargetArtifacts = .{},

/// This is only true once `build` is called and exits successfully
complete: bool = false,

/// Creates a new lld builder from a potentially unfinished LLVM
pub fn init(llvm: *const LLVMBuilder) *Self {
    const b = llvm.b;
    const self = b.allocator.create(Self) catch @panic("OOM");

    self.* = .{
        .b = b,
        .llvm = llvm,
        .metadata = .{
            .root = llvm.metadata.root.dupe(b),
            .lld_include = llvm.metadata.root.path(b, "lld/include"),
            .vcs_version = b.addConfigHeader(.{ .include_path = "VCSVersion.inc" }, .{
                .LLVM_REVISION = LLVMBuilder.llvm_revision,
                .LLD_REVISION = LLVMBuilder.llvm_revision,
                .LLVM_REPOSITORY = "https://github.com/llvm/llvm-project.git",
                .LLD_REPOSITORY = "https://github.com/llvm/llvm-project.git",
            }),
        },
    };
    return self;
}

/// This must be called once llvm has been built with its `build`
pub fn build(self: *Self) void {
    if (!self.llvm.complete) {
        @panic("Misconfigured build script - LLVM was not built");
    }

    self.lld_artifacts.common = self.buildCommon();
    self.lld_artifacts.coff = self.buildCOFF();
    self.lld_artifacts.elf = self.buildELF();
    self.lld_artifacts.macho = self.buildMachO();
    self.lld_artifacts.mingw = self.buildMinGW();
    self.lld_artifacts.wasm = self.buildWasm();

    self.complete = true;
}

/// Creates a module and corresponding library, defaulting to the target platform.
/// `llvm/include` and `lld/include` are implicitly included here
pub fn createLLDLibrary(self: *const Self, config: ArtifactCreateConfig) Artifact {
    const lib = self.llvm.createLLVMLibrary(config);
    lib.root_module.addIncludePath(self.metadata.lld_include);
    return lib;
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/Common/CMakeLists.txt
fn buildCommon(self: *const Self) LLDTargetArtifacts.Common {
    const b = self.b;
    const version_inc = b.addConfigHeader(.{
        .style = .{ .cmake = self.metadata.lld_include.path(b, "lld/Common/Version.inc.in") },
        .include_path = "lld/Common/Version.inc",
    }, .{
        .LLD_VERSION = LLVMBuilder.version_str,
    });

    const config_headers = [_]*std.Build.Step.ConfigHeader{
        version_inc,
        self.metadata.vcs_version,
        self.llvm.metadata.vcs_revision,
    };

    const lib = self.createLLDLibrary(.{
        .name = "lldCommon",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, common.root),
            .files = &common.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
        },
        .config_headers = &config_headers,
        .link_libraries = &.{
            self.llvm.target_artifacts.codegen.core_lib,
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.debug_info.dwarf.core_lib,
            self.llvm.target_artifacts.demangle,
            self.llvm.target_artifacts.machine_code.core_lib,
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.core_lib,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
        },
        .bundle_compiler_rt = true,
    });

    for (config_headers) |config_header| {
        lib.installConfigHeader(config_header);
    }

    return .{
        .core_lib = lib,
        .version_inc = version_inc,
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/COFF/CMakeLists.txt
fn buildCOFF(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, coff.synthesize_options);

    var link_libs = self.llvm.target_artifacts.target_backends.targetsToBuild();
    link_libs.appendSlice(&.{
        self.lld_artifacts.common.core_lib,
        self.llvm.target_artifacts.binary_format,
        self.llvm.target_artifacts.bitcode.writer,
        self.llvm.target_artifacts.core,
        self.llvm.target_artifacts.debug_info.code_view,
        self.llvm.target_artifacts.debug_info.dwarf.core_lib,
        self.llvm.target_artifacts.debug_info.msf,
        self.llvm.target_artifacts.debug_info.pdb,
        self.llvm.target_artifacts.demangle,
        self.llvm.target_artifacts.tool_drivers.lib,
        self.llvm.target_artifacts.lto,
        self.llvm.target_artifacts.dtlto,
        self.llvm.target_artifacts.machine_code.core_lib,
        self.llvm.target_artifacts.object,
        self.llvm.target_artifacts.option,
        self.llvm.target_artifacts.passes,
        self.llvm.target_artifacts.support,
        self.llvm.target_artifacts.target_backends.parser.core_lib,
        self.llvm.target_artifacts.windows_support.driver,
        self.llvm.target_artifacts.windows_support.manifest,
    });

    const root = self.metadata.root.path(b, coff.root);
    const lib = self.createLLDLibrary(.{
        .name = "lldCOFF",
        .cxx_source_files = .{
            .root = root,
            .files = &coff.sources,
        },
        .additional_include_paths = &.{
            root,
            registry.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
        },
        .link_libraries = link_libs.wrapped.items,
        .bundle_compiler_rt = true,
    });

    return .{ .gen = registry, .core_lib = lib };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/ELF/CMakeLists.txt
fn buildELF(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, elf.synthesize_options);

    var link_libs = self.llvm.target_artifacts.target_backends.targetsToBuild();
    link_libs.appendSlice(&.{
        self.lld_artifacts.common.core_lib,
        self.llvm.target_artifacts.deps.zlib.artifact,
        self.llvm.target_artifacts.deps.zstd.artifact,
        self.llvm.target_artifacts.binary_format,
        self.llvm.target_artifacts.bitcode.writer,
        self.llvm.target_artifacts.core,
        self.llvm.target_artifacts.debug_info.dwarf.core_lib,
        self.llvm.target_artifacts.demangle,
        self.llvm.target_artifacts.tool_drivers.lib,
        self.llvm.target_artifacts.lto,
        self.llvm.target_artifacts.dtlto,
        self.llvm.target_artifacts.machine_code.core_lib,
        self.llvm.target_artifacts.object,
        self.llvm.target_artifacts.option,
        self.llvm.target_artifacts.passes,
        self.llvm.target_artifacts.profile_data.core_lib,
        self.llvm.target_artifacts.support,
        self.llvm.target_artifacts.target_backends.parser.core_lib,
        self.llvm.target_artifacts.transforms.utils,
    });

    const root = self.metadata.root.path(b, elf.root);
    const lib = self.createLLDLibrary(.{
        .name = "lldELF",
        .cxx_source_files = .{
            .root = root,
            .files = &elf.sources,
        },
        .additional_include_paths = &.{
            root,
            registry.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
        },
        .link_libraries = link_libs.wrapped.items,
        .bundle_compiler_rt = true,
    });

    return .{ .gen = registry, .core_lib = lib };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/MachO/CMakeLists.txt
fn buildMachO(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, macho.synthesize_options);

    var link_libs = self.llvm.target_artifacts.target_backends.targetsToBuild();
    link_libs.appendSlice(&.{
        self.lld_artifacts.common.core_lib,
        self.llvm.target_artifacts.binary_format,
        self.llvm.target_artifacts.bitcode.reader,
        self.llvm.target_artifacts.bitcode.writer,
        self.llvm.target_artifacts.codegen.data,
        self.llvm.target_artifacts.core,
        self.llvm.target_artifacts.debug_info.dwarf.core_lib,
        self.llvm.target_artifacts.demangle,
        self.llvm.target_artifacts.lto,
        self.llvm.target_artifacts.machine_code.core_lib,
        self.llvm.target_artifacts.transforms.obj_carc,
        self.llvm.target_artifacts.object,
        self.llvm.target_artifacts.option,
        self.llvm.target_artifacts.passes,
        self.llvm.target_artifacts.profile_data.core_lib,
        self.llvm.target_artifacts.support,
        self.llvm.target_artifacts.target_backends.parser.core_lib,
        self.llvm.target_artifacts.text_api.core_lib,
    });

    const root = self.metadata.root.path(b, macho.root);
    const lib = self.createLLDLibrary(.{
        .name = "lldMachO",
        .cxx_source_files = .{
            .root = root,
            .files = &macho.sources,
        },
        .additional_include_paths = &.{
            root,
            self.metadata.root.path(b, "libunwind/include"),
            registry.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
        },
        .link_libraries = link_libs.wrapped.items,
    });

    return .{ .gen = registry, .core_lib = lib };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/MinGW/CMakeLists.txt
fn buildMinGW(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, mingw.synthesize_options);

    const lib = self.createLLDLibrary(.{
        .name = "lldMinGW",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, mingw.root),
            .files = &mingw.sources,
        },
        .additional_include_paths = &.{
            registry.getDirectory(),
            self.lld_artifacts.coff.gen.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
        },
        .link_libraries = &.{
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.lld_artifacts.coff.core_lib,
            self.lld_artifacts.common.core_lib,
        },
    });

    return .{ .gen = registry, .core_lib = lib };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/wasm/CMakeLists.txt
fn buildWasm(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, wasm.synthesize_options);

    var link_libs = self.llvm.target_artifacts.target_backends.targetsToBuild();
    link_libs.appendSlice(&.{
        self.lld_artifacts.common.core_lib,
        self.llvm.target_artifacts.binary_format,
        self.llvm.target_artifacts.bitcode.writer,
        self.llvm.target_artifacts.core,
        self.llvm.target_artifacts.demangle,
        self.llvm.target_artifacts.lto,
        self.llvm.target_artifacts.machine_code.core_lib,
        self.llvm.target_artifacts.object,
        self.llvm.target_artifacts.option,
        self.llvm.target_artifacts.passes,
        self.llvm.target_artifacts.profile_data.core_lib,
        self.llvm.target_artifacts.support,
        self.llvm.target_artifacts.target_backends.parser.core_lib,
    });

    const root = self.metadata.root.path(b, wasm.root);
    const lib = self.createLLDLibrary(.{
        .name = "lldWasm",
        .cxx_source_files = .{
            .root = root,
            .files = &wasm.sources,
        },
        .additional_include_paths = &.{
            root,
            registry.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
        },
        .link_libraries = link_libs.wrapped.items,
    });

    return .{ .gen = registry, .core_lib = lib };
}

pub fn allArtifacts(self: *const Self) []Artifact {
    return stdx.ArrayList(Artifact).fromSlice(self.b, &.{
        self.lld_artifacts.common.core_lib,
        self.lld_artifacts.coff.core_lib,
        self.lld_artifacts.elf.core_lib,
        self.lld_artifacts.macho.core_lib,
        self.lld_artifacts.mingw.core_lib,
        self.lld_artifacts.wasm.core_lib,
    }).wrapped.items;
}

pub fn allIncludePaths(self: *const Self) LLVMBuilder.AllIncludes {
    const all_includes: stdx.ArrayList(std.Build.LazyPath) = .fromSlice(self.b, &.{
        self.metadata.lld_include,
        self.lld_artifacts.coff.gen.getDirectory(),
        self.lld_artifacts.elf.gen.getDirectory(),
        self.lld_artifacts.macho.gen.getDirectory(),
        self.lld_artifacts.mingw.gen.getDirectory(),
        self.lld_artifacts.wasm.gen.getDirectory(),
    });

    const all_config_headers: stdx.ArrayList(*std.Build.Step.ConfigHeader) = .fromSlice(self.b, &.{
        self.metadata.vcs_version,
        self.lld_artifacts.common.version_inc,
    });

    return .{
        .includes = all_includes.wrapped.items,
        .config_headers = all_config_headers.wrapped.items,
    };
}
