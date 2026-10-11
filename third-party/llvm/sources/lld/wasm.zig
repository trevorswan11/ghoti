//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/wasm/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "lld/wasm/";
pub const sources = [_][]const u8{
    "Driver.cpp",
    "InputChunks.cpp",
    "InputFiles.cpp",
    "LTO.cpp",
    "MapFile.cpp",
    "MarkLive.cpp",
    "OutputSections.cpp",
    "OutputSegment.cpp",
    "Relocations.cpp",
    "SymbolTable.cpp",
    "Symbols.cpp",
    "SyntheticSections.cpp",
    "Writer.cpp",
    "WriterUtils.cpp",
};

pub const synthesize_options: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "WasmOptionsTableGen",
        .td_file = root ++ "Options.td",
        .instruction = .{ .action = "-gen-opt-parser-defs" },
    },
    .virtual_path = "Options.inc",
};
