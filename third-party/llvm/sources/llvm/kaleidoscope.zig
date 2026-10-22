//! https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/llvm/examples/Kaleidoscope
const std = @import("std");

const LLVMBuilder = @import("../../LLVMBuilder.zig");

const kaleidoscope_root = "llvm/examples/Kaleidoscope";

pub const KaleidoscopeChapter = enum {
    BuildingAJIT_Ch1,
    BuildingAJIT_Ch2,
    BuildingAJIT_Ch3,
    BuildingAJIT_Ch4,

    Chapter2,
    Chapter3,
    Chapter4,
    Chapter5,
    Chapter6,
    Chapter7,
    Chapter8,
    Chapter9,

    /// Returns the directory name of the Chapter relative to the kaleidoscope root.
    fn relativeDirectory(self: KaleidoscopeChapter) []const u8 {
        return switch (self) {
            .BuildingAJIT_Ch1 => "BuildingAJIT/Chapter1",
            .BuildingAJIT_Ch2 => "BuildingAJIT/Chapter2",
            .BuildingAJIT_Ch3 => "BuildingAJIT/Chapter3",
            .BuildingAJIT_Ch4 => "BuildingAJIT/Chapter4",
            else => @tagName(self),
        };
    }

    /// The chapter-specific include directory
    fn includeDirectory(self: KaleidoscopeChapter, llvm: *const LLVMBuilder) std.Build.LazyPath {
        const b = llvm.b;
        const kaleidoscope_path = llvm.metadata.root.path(b, kaleidoscope_root);
        return switch (self) {
            .BuildingAJIT_Ch1,
            .BuildingAJIT_Ch2,
            .BuildingAJIT_Ch3,
            .BuildingAJIT_Ch4,
            => kaleidoscope_path.path(b, self.relativeDirectory()),
            else => kaleidoscope_path,
        };
    }

    /// Compiles the chapter of Kaleidoscope. The host must match the target.
    pub fn build(self: KaleidoscopeChapter, llvm: *const LLVMBuilder) *std.Build.Step.Compile {
        const b = llvm.b;
        if (!llvm.complete) {
            @panic("Misconfigured build script - LLVM was not built");
        }

        const mod = b.createModule(.{
            .target = b.graph.host,
            .optimize = .Debug,
            .link_libc = true,
            .link_libcpp = true,
        });

        const source_file = b.fmt("{s}/{s}/toy.cpp", .{ kaleidoscope_root, self.relativeDirectory() });
        mod.addCSourceFile(.{
            .file = llvm.metadata.root.path(b, source_file),
            .flags = &LLVMBuilder.common_llvm_cxx_flags,
        });

        const include = self.includeDirectory(llvm);
        const includes = llvm.allIncludePaths();
        for (includes.includes) |llvm_include| {
            mod.addSystemIncludePath(llvm_include);
        }
        mod.addIncludePath(include);

        for (includes.config_headers) |config_header| {
            mod.addConfigHeader(config_header);
        }

        for (llvm.allTargetArtifacts()) |artifact| {
            mod.linkLibrary(artifact);
        }

        return b.addExecutable(.{
            .name = b.fmt("Kaleidoscope{s}", .{@tagName(self)}),
            .root_module = mod,
        });
    }
};
