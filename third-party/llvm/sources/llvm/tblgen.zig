/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/TableGen/CMakeLists.txt
pub const lib_root = "llvm/lib/TableGen/";
pub const lib_sources = [_][]const u8{
    "DetailedRecordsBackend.cpp",
    "Error.cpp",
    "JSONBackend.cpp",
    "Main.cpp",
    "Parser.cpp",
    "Record.cpp",
    "SetTheory.cpp",
    "StringMatcher.cpp",
    "StringToOffsetTable.cpp",
    "TableGenBackend.cpp",
    "TableGenBackendSkeleton.cpp",
    "TGLexer.cpp",
    "TGParser.cpp",
    "TGTimer.cpp",
};

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/utils/TableGen/Basic/CMakeLists.txt
pub const basic_root = utils_root ++ "Basic/";
pub const basic_sources = [_][]const u8{
    "ARMTargetDefEmitter.cpp",
    "Attributes.cpp",
    "CodeGenIntrinsics.cpp",
    "DirectiveEmitter.cpp",
    "IntrinsicEmitter.cpp",
    "RISCVTargetDefEmitter.cpp",
    "RuntimeLibcallsEmitter.cpp",
    "RuntimeLibcalls.cpp",
    "SDNodeProperties.cpp",
    "TableGen.cpp",
    "TargetFeaturesEmitter.cpp",
    "TargetLibraryInfoEmitter.cpp",
    "VTEmitter.cpp",
};

pub const minimal_main = utils_root ++ "llvm-min-tblgen.cpp";

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/utils/TableGen/CMakeLists.txt
pub const common_root = utils_root ++ "Common/";
pub const common_sources = [_][]const u8{
    "GlobalISel/CodeExpander.cpp",
    "GlobalISel/CombinerUtils.cpp",
    "GlobalISel/CXXPredicates.cpp",
    "GlobalISel/GlobalISelMatchTableExecutorEmitter.cpp",
    "GlobalISel/PatternParser.cpp",
    "GlobalISel/Patterns.cpp",
    "AsmWriterInst.cpp",
    "CodeGenDAGPatterns.cpp",
    "CodeGenHwModes.cpp",
    "CodeGenInstAlias.cpp",
    "CodeGenInstruction.cpp",
    "CodeGenRegisters.cpp",
    "CodeGenSchedule.cpp",
    "CodeGenTarget.cpp",
    "InfoByHwMode.cpp",
    "InstructionEncoding.cpp",
    "OptEmitter.cpp",
    "PredicateExpander.cpp",
    "SubtargetFeatureInfo.cpp",
    "Types.cpp",
    "Utils.cpp",
    "VarLenCodeEmitterGen.cpp",
    "GlobalISel/MatchTable/MatchTable.cpp",
    "GlobalISel/MatchTable/Matchers.cpp",
    "GlobalISel/MatchTable/Types.cpp",
};

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/utils/TableGen/CMakeLists.txt
pub const utils_root = "llvm/utils/TableGen/";
pub const utils_sources = [_][]const u8{
    "AsmMatcherEmitter.cpp",
    "AsmWriterEmitter.cpp",
    "CallingConvEmitter.cpp",
    "CodeEmitterGen.cpp",
    "CodeGenMapTable.cpp",
    "CompressInstEmitter.cpp",
    "CTagsEmitter.cpp",
    "DAGISelEmitter.cpp",
    "DAGISelMatcher.cpp",
    "DAGISelMatcherEmitter.cpp",
    "DAGISelMatcherGen.cpp",
    "DAGISelMatcherOpt.cpp",
    "DecoderEmitter.cpp",
    "DecoderTableEmitter.cpp",
    "DecoderTree.cpp",
    "DFAEmitter.cpp",
    "DFAPacketizerEmitter.cpp",
    "DisassemblerEmitter.cpp",
    "DXILEmitter.cpp",
    "ExegesisEmitter.cpp",
    "FastISelEmitter.cpp",
    "GlobalISelCombinerEmitter.cpp",
    "GlobalISelEmitter.cpp",
    "InstrDocsEmitter.cpp",
    "InstrInfoEmitter.cpp",
    "llvm-tblgen.cpp",
    "MacroFusionPredicatorEmitter.cpp",
    "OptionParserEmitter.cpp",
    "OptionRSTEmitter.cpp",
    "PseudoLoweringEmitter.cpp",
    "RegisterBankEmitter.cpp",
    "RegisterInfoEmitter.cpp",
    "SDNodeInfoEmitter.cpp",
    "SearchableTableEmitter.cpp",
    "SubtargetEmitter.cpp",
    "WebAssemblyDisassemblerEmitter.cpp",
    "X86InstrMappingEmitter.cpp",
    "X86DisassemblerTables.cpp",
    "X86FoldTablesEmitter.cpp",
    "X86MnemonicTables.cpp",
    "X86ModRMFilters.cpp",
    "X86RecognizableInstr.cpp",
};

const TblgenTargetAction = struct {
    name: []const u8,
    flag: []const u8,
};

pub const actions = [_]TblgenTargetAction{
    .{ .name = "GenRegisterInfo", .flag = "-gen-register-info" },
    .{ .name = "GenInstrInfo", .flag = "-gen-instr-info" },
    .{ .name = "GenAsmWriter", .flag = "-gen-asm-writer" },
    .{ .name = "GenAsmMatcher", .flag = "-gen-asm-matcher" },
    .{ .name = "GenDAGISel", .flag = "-gen-dag-isel" },
    .{ .name = "GenSubtargetInfo", .flag = "-gen-subtarget" },
    .{ .name = "GenCallingConv", .flag = "-gen-callingconv" },
};
