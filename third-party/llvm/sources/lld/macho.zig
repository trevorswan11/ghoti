//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/MachO/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "lld/MachO/";
pub const sources = [_][]const u8{
    "Arch/ARM64.cpp",
    "Arch/ARM64Common.cpp",
    "Arch/ARM64_32.cpp",
    "Arch/X86_64.cpp",
    "ConcatOutputSection.cpp",
    "Driver.cpp",
    "DriverUtils.cpp",
    "Dwarf.cpp",
    "EhFrame.cpp",
    "ExportTrie.cpp",
    "ICF.cpp",
    "InputFiles.cpp",
    "InputSection.cpp",
    "LinkerOptimizationHints.cpp",
    "LTO.cpp",
    "MapFile.cpp",
    "MarkLive.cpp",
    "ObjC.cpp",
    "OutputSection.cpp",
    "OutputSegment.cpp",
    "Relocations.cpp",
    "BPSectionOrderer.cpp",
    "SectionPriorities.cpp",
    "Sections.cpp",
    "SymbolTable.cpp",
    "Symbols.cpp",
    "SyntheticSections.cpp",
    "Target.cpp",
    "UnwindInfoSection.cpp",
    "Writer.cpp",
};

pub const synthesize_options: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "MachOOptionsTableGen",
        .td_file = root ++ "Options.td",
        .instruction = .{ .action = "-gen-opt-parser-defs" },
    },
    .virtual_path = "Options.inc",
};
