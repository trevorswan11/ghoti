//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/COFF/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "lld/COFF/";
pub const sources = [_][]const u8{
    "CallGraphSort.cpp",
    "Chunks.cpp",
    "COFFLinkerContext.cpp",
    "DebugTypes.cpp",
    "DLL.cpp",
    "Driver.cpp",
    "DriverUtils.cpp",
    "ICF.cpp",
    "InputFiles.cpp",
    "LLDMapFile.cpp",
    "LTO.cpp",
    "MapFile.cpp",
    "MarkLive.cpp",
    "MinGW.cpp",
    "PDB.cpp",
    "SymbolTable.cpp",
    "Symbols.cpp",
    "Writer.cpp",
};

pub const synthesize_options: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "COFFOptionsTableGen",
        .td_file = root ++ "Options.td",
        .instruction = .{ .action = "-gen-opt-parser-defs" },
    },
    .virtual_path = "Options.inc",
};
