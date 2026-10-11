//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/ELF/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "lld/ELF/";
pub const sources = [_][]const u8{
    "AArch64ErrataFix.cpp",
    "Arch/AArch64.cpp",
    "Arch/AMDGPU.cpp",
    "Arch/ARM.cpp",
    "Arch/AVR.cpp",
    "Arch/Hexagon.cpp",
    "Arch/LoongArch.cpp",
    "Arch/Mips.cpp",
    "Arch/MipsArchTree.cpp",
    "Arch/MSP430.cpp",
    "Arch/PPC.cpp",
    "Arch/PPC64.cpp",
    "Arch/RISCV.cpp",
    "Arch/SPARCV9.cpp",
    "Arch/SystemZ.cpp",
    "Arch/X86.cpp",
    "Arch/X86_64.cpp",
    "ARMErrataFix.cpp",
    "BPSectionOrderer.cpp",
    "CallGraphSort.cpp",
    "DWARF.cpp",
    "Driver.cpp",
    "DriverUtils.cpp",
    "EhFrame.cpp",
    "ICF.cpp",
    "InputFiles.cpp",
    "InputSection.cpp",
    "LTO.cpp",
    "LinkerScript.cpp",
    "MapFile.cpp",
    "MarkLive.cpp",
    "OutputSections.cpp",
    "Relocations.cpp",
    "ScriptLexer.cpp",
    "ScriptParser.cpp",
    "SymbolTable.cpp",
    "Symbols.cpp",
    "SyntheticSections.cpp",
    "Target.cpp",
    "Thunks.cpp",
    "Writer.cpp",
};

pub const synthesize_options: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "ELFOptionsTableGen",
        .td_file = root ++ "Options.td",
        .instruction = .{ .action = "-gen-opt-parser-defs" },
    },
    .virtual_path = "Options.inc",
};
