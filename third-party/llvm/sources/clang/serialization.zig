//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Serialization/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

const basic = @import("basic.zig");

pub const root = "clang/lib/Serialization";
pub const sources = [_][]const u8{
    "ASTCommon.cpp",
    "ASTReader.cpp",
    "ASTReaderDecl.cpp",
    "ASTReaderStmt.cpp",
    "ASTWriter.cpp",
    "ASTWriterDecl.cpp",
    "ASTWriterStmt.cpp",
    "GeneratePCH.cpp",
    "GlobalModuleIndex.cpp",
    "InMemoryModuleCache.cpp",
    "ModuleCache.cpp",
    "ModuleFile.cpp",
    "ModuleFileExtension.cpp",
    "ModuleManager.cpp",
    "PCHContainerOperations.cpp",
    "ObjectFilePCHContainerReader.cpp",
    "TemplateArgumentHasher.cpp",
};

pub const attr_td = basic.attr_td;

pub const attr_synthesize_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "ClangAttrPCHRead",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-pch-read" },
        },
        .virtual_path = "clang/Serialization/AttrPCHRead.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrPCHWrite",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-pch-write" },
        },
        .virtual_path = "clang/Serialization/AttrPCHWrite.inc",
    },
};
