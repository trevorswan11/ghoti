//! Clang Source Compilation rules. All artifacts are compiled as ReleaseSafe.
const std = @import("std");

const LLVMBuilder = @import("LLVMBuilder.zig");

const stdx = LLVMBuilder.stdx;
const analysis = @import("sources/clang/analysis.zig");
const api_notes = @import("sources/clang/api_notes.zig");
const ast = @import("sources/clang/ast.zig");
const basic = @import("sources/clang/basic.zig");
const codegen = @import("sources/clang/codegen.zig");
const driver = @import("sources/clang/driver.zig");
const extract_api = @import("sources/clang/extract_api.zig");
const format = @import("sources/clang/format.zig");
const frontend = @import("sources/clang/frontend.zig");
const index = @import("sources/clang/index.zig");
const install_api = @import("sources/clang/install_api.zig");
const lex = @import("sources/clang/lex.zig");
const parse = @import("sources/clang/parse.zig");
const sema = @import("sources/clang/sema.zig");
const serialization = @import("sources/clang/serialization.zig");
const static_analyzer = @import("sources/clang/static_analyzer.zig");
const tblgen = @import("sources/clang/tblgen.zig");
const tooling = @import("sources/clang/tooling.zig");

const Artifact = LLVMBuilder.Artifact;
const ArtifactCreateConfig = LLVMBuilder.ArtifactCreateConfig;
const ArtifactWithGen = LLVMBuilder.ArtifactWithGen;

const Metadata = struct {
    root: std.Build.LazyPath,
    clang_include: std.Build.LazyPath,
    vcs_version: *std.Build.Step.ConfigHeader,
};

/// Artifacts compiled for the actual target, associated with the clang subproject of llvm
const ClangTargetArtifacts = struct {
    const Basic = struct {
        gen: *std.Build.Step.WriteFile = undefined,
        core_lib: Artifact = undefined,
        version_inc: *std.Build.Step.ConfigHeader = undefined,
    };

    const ToolingInclusions = struct {
        core_lib: Artifact = undefined,
        stdlib: Artifact = undefined,
    };

    const Tooling = struct {
        /// This is really `clangTooling`
        core_lib: Artifact = undefined,

        /// This is really `clangToolingCore`
        core: Artifact = undefined,
        ast_diff: Artifact = undefined,
        dep_scanning: Artifact = undefined,
        inclusions: ToolingInclusions = .{},
        refactoring: Artifact = undefined,
        syntax: ArtifactWithGen = undefined,
        transformer: Artifact = undefined,
    };

    const StaticAnalyzer = struct {
        gen: *std.Build.Step.WriteFile = undefined,
        checkers: Artifact = undefined,
        core: Artifact = undefined,
        frontend: Artifact = undefined,
    };

    const AST = struct {
        gen: *std.Build.Step.WriteFile = undefined,
        core_lib: Artifact = undefined,
        matchers: struct {
            core_lib: Artifact = undefined,
            dynamic: Artifact = undefined,
        } = .{},
    };

    const Frontend = struct {
        core_lib: Artifact = undefined,
        rewrite: Artifact = undefined,
        tool: Artifact = undefined,
    };

    const ScalableStaticAnalysis = struct {
        core: Artifact = undefined,
        analyses: Artifact = undefined,
        frontend: Artifact = undefined,
        source_transformation: Artifact = undefined,
    };

    support: Artifact = undefined,
    options: ArtifactWithGen = .{},
    driver: ArtifactWithGen = .{},
    sema: ArtifactWithGen = .{},
    basic: Basic = .{},
    lex: Artifact = undefined,
    rewrite: Artifact = undefined,

    analysis: Artifact = undefined,
    analysis_lifetime_safety: Artifact = undefined,
    unified_symbol_resolution: Artifact = undefined,
    scalable_static_analysis: ScalableStaticAnalysis = .{},
    api_notes: Artifact = undefined,
    ast: AST = .{},
    codegen: Artifact = undefined,
    cross_tu: Artifact = undefined,
    edit: Artifact = undefined,
    extract_api: Artifact = undefined,
    format: Artifact = undefined,
    frontend: Frontend = undefined,
    index: Artifact = undefined,
    install_api: Artifact = undefined,
    parse: ArtifactWithGen = .{},
    serialization: ArtifactWithGen = .{},
    static_analyzer: StaticAnalyzer = .{},
    tooling: Tooling = .{},
};

/// CLI Tools provided by Clang
const ClangTools = struct {
    clang_format: Artifact = undefined,
};

const Self = @This();

b: *std.Build,
llvm: *const LLVMBuilder,
metadata: Metadata,

config_h: *std.Build.Step.ConfigHeader = undefined,
clang_artifacts: ClangTargetArtifacts = .{},
clang_tblgen: Artifact = undefined,
clang_tools: ClangTools = .{},

/// This is only true once `build` is called and exits successfully
complete: bool = false,

/// Creates a new clang builder from a potentially unfinished LLVM
pub fn init(llvm: *const LLVMBuilder) *Self {
    const b = llvm.b;
    const self = b.allocator.create(Self) catch @panic("OOM");

    self.* = .{
        .b = b,
        .llvm = llvm,
        .metadata = .{
            .root = llvm.metadata.root.dupe(b),
            .clang_include = llvm.metadata.root.path(b, "clang/include"),
            .vcs_version = b.addConfigHeader(.{ .include_path = "VCSVersion.inc" }, .{
                .LLVM_REVISION = LLVMBuilder.llvm_revision,
                .CLANG_REVISION = LLVMBuilder.llvm_revision,
                .LLVM_REPOSITORY = "https://github.com/llvm/llvm-project.git",
                .CLANG_REPOSITORY = "https://github.com/llvm/llvm-project.git",
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

    self.clang_artifacts.support = self.buildSupport();
    self.clang_tblgen = self.buildTblgen();
    self.config_h = self.createConfigH();

    self.clang_artifacts.sema.gen = self.runSemaGen();
    self.clang_artifacts.basic.gen = self.runBasicGen();
    self.clang_artifacts.basic.core_lib = self.buildBasic();
    self.clang_artifacts.lex = self.buildLex();
    self.clang_artifacts.rewrite = self.buildRewrite();
    self.clang_artifacts.tooling.core = self.buildToolingCore();
    self.clang_artifacts.tooling.inclusions.core_lib = self.buildToolingInclusions();
    self.clang_artifacts.format = self.buildFormat();

    self.clang_artifacts.static_analyzer.gen = self.runStaticAnalysisGen();
    self.clang_artifacts.options = self.buildOptions();
    self.clang_artifacts.api_notes = self.buildAPINotes();
    self.clang_artifacts.ast = self.buildAST();
    self.clang_artifacts.analysis = self.buildAnalysis();
    self.clang_artifacts.analysis_lifetime_safety = self.buildAnalysisLifetimeSafety();
    self.clang_artifacts.edit = self.buildEdit();
    self.clang_artifacts.sema.core_lib = self.buildSema();

    self.clang_artifacts.serialization = self.buildSerialization();
    self.clang_artifacts.parse = self.buildParse();
    self.clang_artifacts.frontend.core_lib = self.buildFrontendCore();
    self.clang_artifacts.unified_symbol_resolution = self.buildUnifiedSymbolResolution();
    self.clang_artifacts.install_api = self.buildInstallAPI();
    self.clang_artifacts.index = self.buildIndex();
    self.clang_artifacts.extract_api = self.buildExtractAPI();
    self.clang_artifacts.frontend.rewrite = self.buildFrontendRewrite();
    self.clang_artifacts.codegen = self.buildCodeGen();
    self.clang_artifacts.tooling.dep_scanning = self.buildDependencyScanning();
    self.clang_artifacts.scalable_static_analysis = self.buildScalableStaticAnalysis();
    self.clang_artifacts.driver = self.buildDriver();
    self.clang_artifacts.cross_tu = self.buildCrossTU();
    self.clang_artifacts.static_analyzer.core = self.buildSACore();
    self.clang_artifacts.static_analyzer.checkers = self.buildSACheckers();
    self.clang_artifacts.static_analyzer.frontend = self.buildSAFrontend();
    self.clang_artifacts.frontend.tool = self.buildFrontendTool();

    self.clang_artifacts.tooling.ast_diff = self.buildToolingASTDiff();
    self.clang_artifacts.tooling.core_lib = self.buildTooling();
    self.clang_artifacts.tooling.inclusions.stdlib = self.buildToolingInclusionsStdlib();
    self.clang_artifacts.tooling.refactoring = self.buildToolingRefactoring();
    self.clang_artifacts.tooling.syntax = self.buildToolingSyntax();
    self.clang_artifacts.tooling.transformer = self.buildToolingTransformer();

    self.clang_tools.clang_format = self.buildFormatTool();

    self.complete = true;
}

/// Creates a module and corresponding library, defaulting to the target platform.
/// `llvm/include` and `clang/include` are implicitly included here
pub fn createClangLibrary(self: *const Self, config: ArtifactCreateConfig) Artifact {
    const lib = self.llvm.createLLVMLibrary(config);
    lib.root_module.addIncludePath(self.metadata.clang_include);
    return lib;
}

/// Creates a module and corresponding executable, defaulting to the target platform.
/// `llvm/include` and `clang/include` are implicitly included here
pub fn createClangExecutable(self: *const Self, config: ArtifactCreateConfig) Artifact {
    const exe = self.llvm.createLLVMExecutable(config);
    exe.root_module.addIncludePath(self.metadata.clang_include);
    return exe;
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Config/config.h.cmake
fn createConfigH(self: *const Self) *std.Build.Step.ConfigHeader {
    const b = self.b;
    const is_windows = self.llvm.target.result.os.tag == .windows;

    return b.addConfigHeader(
        .{
            .style = .{ .cmake = self.metadata.clang_include.path(b, "clang/Config/config.h.cmake") },
            .include_path = "clang/Config/config.h",
        },
        .{
            .BUG_REPORT_URL = "https://github.com/llvm/llvm-project/issues",
            .CLANG_DEFAULT_PIE_ON_LINUX = 0,
            .CLANG_DEFAULT_LINKER = "", // TODO: Set to lld when building lld
            .CLANG_DEFAULT_CXX_STDLIB = "",
            .CLANG_DEFAULT_RTLIB = "",
            .CLANG_DEFAULT_UNWINDLIB = "",
            .CLANG_DEFAULT_OBJCOPY = "objcopy",
            .CLANG_DEFAULT_OPENMP_RUNTIME = "libomp",
            .CLANG_SYSTEMZ_DEFAULT_ARCH = "z10",
            .CLANG_INSTALL_LIBDIR_BASENAME = "lib",
            .CLANG_RESOURCE_DIR = "",
            .C_INCLUDE_DIRS = "",
            .CLANG_CONFIG_FILE_SYSTEM_DIR = null,
            .CLANG_CONFIG_FILE_USER_DIR = null,
            .DEFAULT_SYSROOT = "",
            .GCC_INSTALL_PREFIX = "",
            .CLANG_HAVE_LIBXML = 1,
            .CLANG_HAVE_RLIMITS = @intFromBool(!is_windows),
            .CLANG_HAVE_DLFCN_H = @intFromBool(!is_windows),
            .CLANG_HAVE_DLADDR = @intFromBool(!is_windows),
            .HOST_LINK_VERSION = null,
            .ENABLE_LINKER_BUILD_ID = null,
            .ENABLE_X86_RELAX_RELOCATIONS = 0,
            .PPC_LINUX_DEFAULT_IEEELONGDOUBLE = 0,
            .CLANG_ENABLE_OBJC_REWRITER = 1,
            .CLANG_ENABLE_STATIC_ANALYZER = 1,
            .CLANG_SPAWN_CC1 = 0,
            .CLANG_ENABLE_CIR = 0,
            .CLANG_USE_XCSELECT = null,
            .CLANG_XCSELECT_HOST_SDK_POLICY = "",
            .CLANG_USE_EXPERIMENTAL_CONST_INTERP = 0,
        },
    );
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Support/CMakeLists.txt
fn buildSupport(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangSupport",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, "clang/lib/Support"),
            .files = &.{"RISCVVIntrinsicUtils.cpp"},
        },
        .link_libraries = &.{self.llvm.target_artifacts.support},
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/utils/TableGen/CMakeLists.txt
fn buildTblgen(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangExecutable(.{
        .name = "ClangTableGen",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, tblgen.root),
            .files = &tblgen.sources,
        },
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.configure_phase_artifacts.lib_full_tblgen,
            self.clang_artifacts.support,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Sema/CMakeLists.txt
fn runSemaGen(self: *const Self) *std.Build.Step.WriteFile {
    const b = self.b;
    const registry = b.addWriteFiles();

    // Diagnostics need direct include access to Basic
    for (sema.attr_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    self.llvm.synthesizeHeader(registry, sema.open_cl_synthesize.with(.{
        .tblgen = self.clang_tblgen,
        .extra_includes = &.{self.metadata.clang_include},
    }));

    return registry;
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Basic/CMakeLists.txt
fn runBasicGen(self: *const Self) *std.Build.Step.WriteFile {
    const b = self.b;
    const registry = b.addWriteFiles();

    // Diagnostics need direct include access to Basic
    for (basic.diag_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.root.path(b, basic.include_root)},
        }));
    }

    // Attributes need clang include
    for (basic.attr_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // Builtins need clang include
    for (basic.builtin_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // Arch specific need direct include access to Basic
    for (basic.arch_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.root.path(b, basic.include_root)},
        }));
    }

    return registry;
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Basic/CMakeLists.txt
fn buildBasic(self: *Self) Artifact {
    const b = self.b;
    const llvm = self.llvm;
    self.clang_artifacts.basic.version_inc = b.addConfigHeader(.{
        .style = .{
            .cmake = self.metadata.root.path(b, basic.include_root ++ "Version.inc.in"),
        },
        .include_path = "clang/Basic/Version.inc",
    }, .{
        .CLANG_VERSION = LLVMBuilder.version_str,
        .CLANG_VERSION_STRING = LLVMBuilder.version_str,
        .CLANG_VERSION_MAJOR = @as(i32, @intCast(LLVMBuilder.version.major)),
        .CLANG_VERSION_MAJOR_STRING = std.fmt.comptimePrint(
            "{d}",
            .{LLVMBuilder.version.major},
        ),
        .CLANG_VERSION_MINOR = @as(i32, @intCast(LLVMBuilder.version.minor)),
        .CLANG_VERSION_PATCHLEVEL = @as(i32, @intCast(LLVMBuilder.version.patch)),
        .MAX_CLANG_ABI_COMPAT_VERSION = @as(i32, @intCast(LLVMBuilder.version.major)),
    });

    return self.createClangLibrary(.{
        .name = "clangBasic",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, basic.root),
            .files = &basic.sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
            llvm.target_artifacts.frontend.gen.getDirectory(),
        },
        .config_headers = &.{
            self.metadata.vcs_version,
            llvm.metadata.vcs_revision,
            self.config_h,
            self.clang_artifacts.basic.version_inc,
        },
        .link_libraries = &.{
            llvm.target_artifacts.support,
            llvm.target_artifacts.target_backends.parser.core_lib,
            llvm.target_artifacts.frontend.open_mp,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Lex/CMakeLists.txt
fn buildLex(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangLex",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, lex.root),
            .files = &lex.sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Rewrite/CMakeLists.txt
fn buildRewrite(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangRewrite",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, "clang/lib/Rewrite"),
            .files = &.{ "HTMLRewrite.cpp", "Rewriter.cpp", "TokenRewriter.cpp" },
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.llvm.target_artifacts.support,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Core/CMakeLists.txt
fn buildToolingCore(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangToolingCore",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, tooling.core_root),
            .files = &tooling.core_sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.rewrite,
            self.llvm.target_artifacts.support,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Inclusions/CMakeLists.txt
fn buildToolingInclusions(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangToolingInclusions",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, tooling.inclusions_root),
            .files = &tooling.inclusions_sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.tooling.core,
            self.llvm.target_artifacts.support,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Format/CMakeLists.txt
fn buildFormat(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangFormat",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, format.root),
            .files = &format.sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.tooling.core,
            self.clang_artifacts.tooling.inclusions.core_lib,
            self.llvm.target_artifacts.support,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/StaticAnalyzer/Checkers/CMakeLists.txt
fn runStaticAnalysisGen(self: *const Self) *std.Build.Step.WriteFile {
    const b = self.b;
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, static_analyzer.synthesize_checkers.with(.{
        .tblgen = self.clang_tblgen,
        .extra_includes = &.{self.metadata.root.path(b, static_analyzer.checkers_include)},
    }));
    return registry;
}

/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Options/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Options/CMakeLists.txt
fn buildOptions(self: *const Self) ArtifactWithGen {
    const b = self.b;

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Options/CMakeLists.txt
    const registry = b.addWriteFiles();
    self.llvm.synthesizeHeader(registry, driver.synthesize_opts);

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Options/CMakeLists.txt
    const lib = self.createClangLibrary(.{
        .name = "clangOptions",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, driver.options_root),
            .files = &driver.options_sources,
        },
        .additional_include_paths = &.{
            registry.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.support,
        },
    });

    return .{
        .gen = registry,
        .core_lib = lib,
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Driver/CMakeLists.txt
fn buildDriver(self: *const Self) ArtifactWithGen {
    const b = self.b;
    const registry = self.clang_artifacts.options.gen;

    const lib = self.createClangLibrary(.{
        .name = "clangDriver",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, driver.root),
            .files = &driver.sources,
        },
        .additional_include_paths = &.{
            registry.getDirectory(),
            self.metadata.root.path(b, driver.root),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.tooling.dep_scanning,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.options.core_lib,
            self.clang_artifacts.scalable_static_analysis.analyses,
            self.clang_artifacts.scalable_static_analysis.core,
            self.clang_artifacts.scalable_static_analysis.frontend,
            self.clang_artifacts.scalable_static_analysis.source_transformation,
            self.clang_artifacts.serialization.core_lib,
            self.llvm.target_artifacts.binary_format,
            self.llvm.target_artifacts.machine_code.core_lib,
            self.llvm.target_artifacts.object,
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.profile_data.core_lib,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.llvm.target_artifacts.windows_support.driver,
        },
    });

    // MSVCToolchain.cpp needs version.dll
    if (self.llvm.target.result.os.tag == .windows) {
        lib.root_module.linkSystemLibrary("version", .{});
    }

    return .{
        .gen = registry,
        .core_lib = lib,
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/APINotes/CMakeLists.txt
fn buildAPINotes(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangAPINotes",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, api_notes.root),
            .files = &api_notes.sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.llvm.target_artifacts.bitcode.reader,
            self.llvm.target_artifacts.bitstream_reader,
            self.llvm.target_artifacts.support,
        },
    });
}

/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/AST/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/AST/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ASTMatchers/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ASTMatchers/Dynamic/CMakeLists.txt
fn buildAST(self: *const Self) ClangTargetArtifacts.AST {
    const b = self.b;
    const registry = b.addWriteFiles();

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/AST/CMakeLists.txt
    for (ast.synthesize_include_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/AST/CMakeLists.txt
    self.llvm.synthesizeHeader(registry, ast.synthesize_attr_doc_table.with(.{
        .tblgen = self.clang_tblgen,
        .extra_includes = &.{self.metadata.clang_include},
    }));
    self.llvm.synthesizeHeader(registry, ast.synthesize_opcodes.with(.{
        .tblgen = self.clang_tblgen,
        .extra_includes = &.{self.metadata.clang_include},
    }));

    const ast_core = self.createClangLibrary(.{
        .name = "clangAST",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, ast.root),
            .files = &ast.sources,
        },
        .additional_include_paths = &.{
            self.clang_artifacts.basic.gen.getDirectory(),
            registry.getDirectory(),
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.llvm.target_artifacts.binary_format,
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.frontend.hlsl,
            self.llvm.target_artifacts.linker,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.llvm.target_artifacts.transforms.ipo,
        },
    });

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ASTMatchers/CMakeLists.txt
    const ast_matchers = self.createClangLibrary(.{
        .name = "clangASTMatchers",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, ast.matchers_root),
            .files = &ast.matchers_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            registry.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            ast_core,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
        },
    });

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ASTMatchers/Dynamic/CMakeLists.txt
    const ast_dynamic_matchers = self.createClangLibrary(.{
        .name = "clangDynamicASTMatchers",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, ast.matchers_dynamic_root),
            .files = &ast.matchers_dynamic_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            registry.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            ast_core,
            ast_matchers,
            self.clang_artifacts.basic.core_lib,
        },
    });

    return .{
        .gen = registry,
        .core_lib = ast_core,
        .matchers = .{
            .core_lib = ast_matchers,
            .dynamic = ast_dynamic_matchers,
        },
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Analysis/CMakeLists.txt
fn buildAnalysis(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangAnalysis",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, analysis.root),
            .files = &analysis.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Analysis/LifetimeSafety/CMakeLists.txt
fn buildAnalysisLifetimeSafety(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangAnalysisLifetimeSafety",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, analysis.lifetime_safety_root),
            .files = &analysis.lifetime_safety_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.basic.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/clang/lib/Edit
fn buildEdit(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangEdit",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, "clang/lib/Edit"),
            .files = &.{ "Commit.cpp", "EditedSource.cpp", "RewriteObjCFoundationAPI.cpp" },
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Sema/CMakeLists.txt
fn buildSema(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangSema",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, sema.root),
            .files = &sema.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.demangle,
            self.llvm.target_artifacts.frontend.hlsl,
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.machine_code.core_lib,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.api_notes,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.analysis_lifetime_safety,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.edit,
            self.clang_artifacts.lex,
            self.clang_artifacts.support,
        },
    });
}

/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Serialization/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Serialization/CMakeLists.txt
fn buildSerialization(self: *const Self) ArtifactWithGen {
    const b = self.b;

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Serialization/CMakeLists.txt
    const registry = b.addWriteFiles();
    for (serialization.attr_synthesize_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Serialization/CMakeLists.txt
    const root = self.metadata.root.path(b, serialization.root);
    const lib = self.createClangLibrary(.{
        .name = "clangSerialization",
        .cxx_source_files = .{
            .root = root,
            .files = &serialization.sources,
        },
        .additional_include_paths = &.{
            root,
            registry.getDirectory(),
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{
            self.config_h,
            self.clang_artifacts.basic.version_inc,
        },
        .link_libraries = &.{
            self.llvm.target_artifacts.bitcode.reader,
            self.llvm.target_artifacts.bitstream_reader,
            self.llvm.target_artifacts.object,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.lex,
            self.clang_artifacts.sema.core_lib,
        },
    });

    return .{
        .gen = registry,
        .core_lib = lib,
    };
}

/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Parse/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Parse/CMakeLists.txt
fn buildParse(self: *const Self) ArtifactWithGen {
    const b = self.b;

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Parse/CMakeLists.txt
    const registry = b.addWriteFiles();
    for (parse.synthesize_include_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Parse/CMakeLists.txt
    const lib = self.createClangLibrary(.{
        .name = "clangParse",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, parse.root),
            .files = &parse.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            registry.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.hlsl,
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.machine_code.core_lib,
            self.llvm.target_artifacts.machine_code.parser,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.sema.core_lib,
        },
    });

    return .{
        .gen = registry,
        .core_lib = lib,
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Frontend/CMakeLists.txt
fn buildFrontendCore(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangFrontend",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, frontend.root),
            .files = &frontend.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.parse.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.bitcode.reader,
            self.llvm.target_artifacts.bitstream_reader,
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.profile_data.core_lib,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.api_notes,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.edit,
            self.clang_artifacts.lex,
            self.clang_artifacts.options.core_lib,
            self.clang_artifacts.parse.core_lib,
            self.clang_artifacts.sema.core_lib,
            self.clang_artifacts.serialization.core_lib,
            self.llvm.target_artifacts.plugins,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/UnifiedSymbolResolution/CMakeLists.txt
fn buildUnifiedSymbolResolution(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangUnifiedSymbolResolution",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, "clang/lib/UnifiedSymbolResolution"),
            .files = &.{"USRGeneration.cpp"},
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/InstallAPI/CMakeLists.txt
fn buildInstallAPI(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangInstallAPI",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, install_api.root),
            .files = &install_api.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.text_api.core_lib,
            self.llvm.target_artifacts.text_api.binary_reader,
            self.llvm.target_artifacts.demangle,
            self.llvm.target_artifacts.core,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Index/CMakeLists.txt
fn buildIndex(self: *const Self) Artifact {
    const root = self.metadata.root.path(self.b, index.root);
    return self.createClangLibrary(.{
        .name = "clangIndex",
        .cxx_source_files = .{
            .root = root,
            .files = &index.sources,
        },
        .additional_include_paths = &.{
            root,
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.sema.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.format,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.sema.core_lib,
            self.clang_artifacts.serialization.core_lib,
            self.clang_artifacts.tooling.core,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ExtractAPI/CMakeLists.txt
fn buildExtractAPI(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangExtractAPI",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, extract_api.root),
            .files = &extract_api.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.install_api,
            self.clang_artifacts.lex,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Frontend/Rewrite/CMakeLists.txt
fn buildFrontendRewrite(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangRewriteFrontend",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, frontend.rewrite_root),
            .files = &frontend.rewrite_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.edit,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.rewrite,
            self.clang_artifacts.serialization.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/CodeGen/CMakeLists.txt
fn buildCodeGen(self: *const Self) Artifact {
    const root = self.metadata.root.path(self.b, codegen.root);
    return self.createClangLibrary(.{
        .name = "clangCodeGen",
        .cxx_source_files = .{
            .root = root,
            .files = &codegen.sources,
        },
        .additional_include_paths = &.{
            root,
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.llvm.configure_phase_artifacts.gen_vt.getDirectory(),
            self.llvm.target_artifacts.intrinsics_gen.getDirectory(),
            self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.llvm.metadata.extension_def.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.abi,
            self.llvm.target_artifacts.transforms.aggressive_inst_combine,
            self.llvm.target_artifacts.analysis,
            self.llvm.target_artifacts.bitcode.reader,
            self.llvm.target_artifacts.bitcode.writer,
            self.llvm.target_artifacts.codegen.core_lib,
            self.llvm.target_artifacts.codegen.types,
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.transforms.coroutines,
            self.llvm.target_artifacts.profile_data.coverage,
            self.llvm.target_artifacts.demangle,
            self.llvm.target_artifacts.extensions,
            self.llvm.target_artifacts.frontend.driver,
            self.llvm.target_artifacts.frontend.hlsl,
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.frontend.offloading,
            self.llvm.target_artifacts.transforms.hip_std_par,
            self.llvm.target_artifacts.transforms.ipo,
            self.llvm.target_artifacts.ir_printer,
            self.llvm.target_artifacts.ir_reader,
            self.llvm.target_artifacts.transforms.inst_combine,
            self.llvm.target_artifacts.transforms.instrumentation,
            self.llvm.target_artifacts.lto,
            self.llvm.target_artifacts.linker,
            self.llvm.target_artifacts.machine_code.core_lib,
            self.llvm.target_artifacts.transforms.obj_carc,
            self.llvm.target_artifacts.object,
            self.llvm.target_artifacts.passes,
            self.llvm.target_artifacts.plugins,
            self.llvm.target_artifacts.profile_data.core_lib,
            self.llvm.target_artifacts.transforms.scalar,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.core_lib,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.llvm.target_artifacts.transforms.utils,

            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.serialization.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/CrossTU/CMakeLists.txt
fn buildCrossTU(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangCrossTU",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, "clang/lib/CrossTU"),
            .files = &.{"CrossTranslationUnit.cpp"},
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.driver.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/StaticAnalyzer/Core/CMakeLists.txt
fn buildSACore(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangStaticAnalyzerCore",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, static_analyzer.core_root),
            .files = &static_analyzer.core_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.cross_tu,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.rewrite,
            self.clang_artifacts.tooling.core,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });
}

/// https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/clang/lib/StaticAnalyzer/Checkers
fn buildSACheckers(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangStaticAnalyzerCheckers",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, static_analyzer.checkers_root),
            .files = &static_analyzer.checkers_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.analysis_lifetime_safety,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.static_analyzer.core,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/StaticAnalyzer/Frontend/CMakeLists.txt
fn buildSAFrontend(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangLibrary(.{
        .name = "clangStaticAnalyzerFrontend",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, static_analyzer.frontend_root),
            .files = &static_analyzer.frontend_sources,
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, static_analyzer.checkers_include),
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.cross_tu,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.static_analyzer.checkers,
            self.clang_artifacts.static_analyzer.core,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/FrontendTool/CMakeLists.txt
fn buildFrontendTool(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangFrontendTool",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, "clang/lib/FrontendTool"),
            .files = &.{"ExecuteCompilerInvocation.cpp"},
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.support,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.codegen,
            self.clang_artifacts.driver.core_lib,
            self.clang_artifacts.extract_api,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.frontend.rewrite,
            self.clang_artifacts.static_analyzer.frontend,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/ASTDiff/CMakeLists.txt
fn buildToolingASTDiff(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangToolingASTDiff",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, tooling.ast_diff_root),
            .files = &tooling.ast_diff_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.lex,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/CMakeLists.txt
fn buildTooling(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangTooling",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, tooling.root),
            .files = &tooling.sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.frontend.open_mp,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.tooling.dep_scanning,
            self.clang_artifacts.driver.core_lib,
            self.clang_artifacts.format,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.options.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.rewrite,
            self.clang_artifacts.serialization.core_lib,
            self.clang_artifacts.tooling.core,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/DependencyScanning/CMakeLists.txt
fn buildDependencyScanning(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangDependencyScanning",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, tooling.dep_scanning_root),
            .files = &tooling.dep_scanning_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.options.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
            self.clang_artifacts.serialization.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.core,
            self.llvm.target_artifacts.option,
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.serialization.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ScalableStaticAnalysis
fn buildScalableStaticAnalysis(self: *const Self) ClangTargetArtifacts.ScalableStaticAnalysis {
    const b = self.b;
    const includes = [_]std.Build.LazyPath{
        self.llvm.target_artifacts.frontend.gen.getDirectory(),
        self.clang_artifacts.options.gen.getDirectory(),
        self.clang_artifacts.basic.gen.getDirectory(),
        self.clang_artifacts.ast.gen.getDirectory(),
        self.clang_artifacts.sema.gen.getDirectory(),
        self.clang_artifacts.static_analyzer.gen.getDirectory(),
        self.llvm.target_artifacts.target_backends.parser.gen.getDirectory(),
    };
    var ssa: ClangTargetArtifacts.ScalableStaticAnalysis = .{};

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ScalableStaticAnalysis/Core/CMakeLists.txt
    ssa.core = self.createClangLibrary(.{
        .name = "clangScalableStaticAnalysisCore",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, static_analyzer.ssa_core_root),
            .files = &static_analyzer.ssa_core_sources,
        },
        .additional_include_paths = &includes,
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt
    ssa.analyses = self.createClangLibrary(.{
        .name = "clangScalableStaticAnalysisAnalyses",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, static_analyzer.ssa_analyses_root),
            .files = &static_analyzer.ssa_analyses_sources,
        },
        .additional_include_paths = &includes,
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.analysis,
            self.clang_artifacts.basic.core_lib,
            ssa.core,
        },
    });

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ScalableStaticAnalysis/Frontend/CMakeLists.txt
    ssa.frontend = self.createClangLibrary(.{
        .name = "clangScalableStaticAnalysisFrontend",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, "clang/lib/ScalableStaticAnalysis/Frontend"),
            .files = &.{"TUSummaryExtractorFrontendAction.cpp"},
        },
        .additional_include_paths = &includes,
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.llvm.target_artifacts.target_backends.parser.core_lib,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.sema.core_lib,
            ssa.analyses,
            ssa.core,
        },
    });

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ScalableStaticAnalysis/SourceTransformation/CMakeLists.txt
    ssa.source_transformation = self.createClangLibrary(.{
        .name = "clangScalableStaticAnalysisSourceTransformation",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, static_analyzer.ssa_source_transformation_root),
            .files = &static_analyzer.ssa_source_transformation_sources,
        },
        .additional_include_paths = &includes,
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.tooling.core,
            ssa.core,
        },
    });

    return ssa;
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Inclusions/Stdlib/CMakeLists.txt
fn buildToolingInclusionsStdlib(self: *const Self) Artifact {
    const root = self.metadata.root.path(self.b, tooling.inclusions_stdlib_root);
    return self.createClangLibrary(.{
        .name = "clangToolingInclusionsStdlib",
        .cxx_source_files = .{
            .root = root,
            .files = &tooling.inclusions_stdlib_sources,
        },
        .additional_include_paths = &.{
            root,
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Refactoring/CMakeLists.txt
fn buildToolingRefactoring(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangToolingRefactoring",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, tooling.refactoring_root),
            .files = &tooling.refactoring_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.format,
            self.clang_artifacts.lex,
            self.clang_artifacts.rewrite,
            self.clang_artifacts.tooling.core,
            self.clang_artifacts.unified_symbol_resolution,
        },
    });
}

/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Tooling/Syntax/CMakeLists.txt
/// - https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Syntax/CMakeLists.txt
fn buildToolingSyntax(self: *const Self) ArtifactWithGen {
    const b = self.b;

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/include/clang/Tooling/Syntax/CMakeLists.txt
    const registry = b.addWriteFiles();
    for (tooling.synthesize_node_configs) |config| {
        self.llvm.synthesizeHeader(registry, config.with(.{
            .tblgen = self.clang_tblgen,
            .extra_includes = &.{self.metadata.clang_include},
        }));
    }

    // https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Syntax/CMakeLists.txt
    const lib = self.createClangLibrary(.{
        .name = "clangToolingSyntax",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, tooling.syntax_root),
            .files = &tooling.syntax_sources,
        },
        .additional_include_paths = &.{
            registry.getDirectory(),
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.frontend.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.tooling.core,
        },
    });

    return .{
        .gen = registry,
        .core_lib = lib,
    };
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Tooling/Transformer/CMakeLists.txt
fn buildToolingTransformer(self: *const Self) Artifact {
    return self.createClangLibrary(.{
        .name = "clangTransformer",
        .cxx_source_files = .{
            .root = self.metadata.root.path(self.b, tooling.transformer_root),
            .files = &tooling.transformer_sources,
        },
        .additional_include_paths = &.{
            self.llvm.target_artifacts.frontend.gen.getDirectory(),
            self.clang_artifacts.basic.gen.getDirectory(),
            self.clang_artifacts.ast.gen.getDirectory(),
            self.clang_artifacts.static_analyzer.gen.getDirectory(),
        },
        .config_headers = &.{self.config_h},
        .link_libraries = &.{
            self.llvm.target_artifacts.frontend.open_acc,
            self.llvm.target_artifacts.support,
            self.clang_artifacts.ast.core_lib,
            self.clang_artifacts.ast.matchers.core_lib,
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.lex,
            self.clang_artifacts.tooling.core,
            self.clang_artifacts.tooling.refactoring,
        },
    });
}

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/tools/clang-format/CMakeLists.txt
fn buildFormatTool(self: *const Self) Artifact {
    const b = self.b;
    return self.createClangExecutable(.{
        .name = "clang-format",
        .cxx_source_files = .{
            .root = self.metadata.root.path(b, "clang/tools/clang-format"),
            .files = &.{"ClangFormat.cpp"},
        },
        .additional_include_paths = &.{
            self.metadata.root.path(b, basic.root),
            self.clang_artifacts.basic.gen.getDirectory(),
        },
        .config_headers = &.{ self.config_h, self.clang_artifacts.basic.version_inc },
        .link_libraries = &.{
            self.clang_artifacts.basic.core_lib,
            self.clang_artifacts.format,
            self.clang_artifacts.rewrite,
            self.clang_artifacts.tooling.core,
            self.llvm.target_artifacts.support,
        },
    });
}

/// Returns all clang-specific artifacts
pub fn allArtifacts(self: *const Self) []Artifact {
    return stdx.ArrayList(Artifact).fromSlice(self.b, &.{
        self.clang_artifacts.support,
        self.clang_artifacts.options.core_lib,
        self.clang_artifacts.driver.core_lib,
        self.clang_artifacts.sema.core_lib,
        self.clang_artifacts.basic.core_lib,
        self.clang_artifacts.lex,
        self.clang_artifacts.rewrite,
        self.clang_artifacts.analysis,
        self.clang_artifacts.analysis_lifetime_safety,
        self.clang_artifacts.unified_symbol_resolution,
        self.clang_artifacts.scalable_static_analysis.core,
        self.clang_artifacts.scalable_static_analysis.analyses,
        self.clang_artifacts.scalable_static_analysis.frontend,
        self.clang_artifacts.scalable_static_analysis.source_transformation,
        self.clang_artifacts.api_notes,
        self.clang_artifacts.ast.core_lib,
        self.clang_artifacts.ast.matchers.core_lib,
        self.clang_artifacts.ast.matchers.dynamic,
        self.clang_artifacts.codegen,
        self.clang_artifacts.cross_tu,
        self.clang_artifacts.edit,
        self.clang_artifacts.extract_api,
        self.clang_artifacts.format,
        self.clang_artifacts.frontend.core_lib,
        self.clang_artifacts.frontend.rewrite,
        self.clang_artifacts.frontend.tool,
        self.clang_artifacts.index,
        self.clang_artifacts.install_api,
        self.clang_artifacts.parse.core_lib,
        self.clang_artifacts.serialization.core_lib,
        self.clang_artifacts.static_analyzer.checkers,
        self.clang_artifacts.static_analyzer.core,
        self.clang_artifacts.static_analyzer.frontend,
        self.clang_artifacts.tooling.ast_diff,
        self.clang_artifacts.tooling.core,
        self.clang_artifacts.tooling.core_lib,
        self.clang_artifacts.tooling.dep_scanning,
        self.clang_artifacts.tooling.inclusions.core_lib,
        self.clang_artifacts.tooling.inclusions.stdlib,
        self.clang_artifacts.tooling.refactoring,
        self.clang_artifacts.tooling.syntax.core_lib,
        self.clang_artifacts.tooling.transformer,
    }).wrapped.items;
}

/// Populates all clang-specific include and config header paths for inclusion in modules
pub fn allIncludePaths(self: *const Self) LLVMBuilder.AllIncludes {
    const all_includes: stdx.ArrayList(std.Build.LazyPath) = .fromSlice(self.b, &.{
        self.metadata.clang_include,
        self.clang_artifacts.basic.gen.getDirectory(),
        self.clang_artifacts.static_analyzer.gen.getDirectory(),
        self.clang_artifacts.ast.gen.getDirectory(),
        self.clang_artifacts.options.gen.getDirectory(),
        self.clang_artifacts.sema.gen.getDirectory(),
        self.clang_artifacts.parse.gen.getDirectory(),
        self.clang_artifacts.serialization.gen.getDirectory(),
        self.clang_artifacts.tooling.syntax.gen.getDirectory(),
    });

    const all_config_headers: stdx.ArrayList(*std.Build.Step.ConfigHeader) = .fromSlice(self.b, &.{
        self.metadata.vcs_version,
        self.config_h,
        self.clang_artifacts.basic.version_inc,
    });

    return .{
        .includes = all_includes.wrapped.items,
        .config_headers = all_config_headers.wrapped.items,
    };
}
