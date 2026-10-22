//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/Target
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "llvm/lib/Target/";
pub const base_sources = [_][]const u8{
    "RegisterTargetPassConfigCallback.cpp",
    "Target.cpp",
    "TargetLoweringObjectFile.cpp",
    "TargetMachine.cpp",
    "TargetMachineC.cpp",
};

pub const parser_root = "llvm/lib/TargetParser";
pub const parser_sources = [_][]const u8{
    "AArch64TargetParser.cpp",
    "AMDGPUTargetParser.cpp",
    "ARMTargetParserCommon.cpp",
    "ARMTargetParser.cpp",
    "AVRTargetParser.cpp",
    "CSKYTargetParser.cpp",
    "Host.cpp",
    "LoongArchTargetParser.cpp",
    "PPCTargetParser.cpp",
    "RISCVISAInfo.cpp",
    "RISCVTargetParser.cpp",
    "SubtargetFeature.cpp",
    "TargetParser.cpp",
    "Triple.cpp",
    "TargetDataLayout.cpp",
    "X86TargetParser.cpp",
    "XtensaTargetParser.cpp",
};

const SythesizeWithInclude = struct {
    config: SynthesizeHeaderConfig,
    include_path: []const u8,
};

pub const parser_synthesize_pairs = [_]SythesizeWithInclude{
    .{
        .config = .{
            .gen_conf = .{
                .name = "AArch64TargetParserDef",
                .td_file = "llvm/lib/Target/AArch64/AArch64.td",
                .instruction = .{ .action = "-gen-arm-target-def" },
            },
            .virtual_path = "llvm/TargetParser/AArch64TargetParserDef.inc",
        },
        .include_path = "llvm/lib/Target/AArch64",
    },
    .{
        .config = .{
            .gen_conf = .{
                .name = "ARMTargetParserDef",
                .td_file = "llvm/lib/Target/ARM/ARM.td",
                .instruction = .{ .action = "-gen-arm-target-def" },
            },
            .virtual_path = "llvm/TargetParser/ARMTargetParserDef.inc",
        },
        .include_path = "llvm/lib/Target/ARM",
    },
    .{
        .config = .{
            .gen_conf = .{
                .name = "RISCVTargetParserDef",
                .td_file = "llvm/lib/Target/RISCV/RISCV.td",
                .instruction = .{ .action = "-gen-riscv-target-def" },
            },
            .virtual_path = "llvm/TargetParser/RISCVTargetParserDef.inc",
        },
        .include_path = "llvm/lib/Target/RISCV",
    },

    .{
        .config = .{
            .gen_conf = .{
                .name = "PPCGenTargetFeatures",
                .td_file = "llvm/lib/Target/PowerPC/PPC.td",
                .instruction = .{ .action = "-gen-target-features" },
            },
            .virtual_path = "llvm/TargetParser/PPCGenTargetFeatures.inc",
        },
        .include_path = "llvm/lib/Target/PowerPC",
    },
};

const BackendAction = struct {
    name: []const u8,
    td_args: []const []const u8,
    /// Overrides the backend's .td file
    td_file: ?[]const u8 = null,
};

pub const AArch64 = struct {
    pub const backend_root = root ++ "AArch64/";
    pub const td_filepath = backend_root ++ "AArch64.td";
    pub const backend_sources = [_][]const u8{
        "GISel/AArch64CallLowering.cpp",
        "GISel/AArch64GlobalISelUtils.cpp",
        "GISel/AArch64InstructionSelector.cpp",
        "GISel/AArch64LegalizerInfo.cpp",
        "GISel/AArch64O0PreLegalizerCombiner.cpp",
        "GISel/AArch64PreLegalizerCombiner.cpp",
        "GISel/AArch64PostLegalizerCombiner.cpp",
        "GISel/AArch64PostLegalizerLowering.cpp",
        "GISel/AArch64PostSelectOptimize.cpp",
        "GISel/AArch64RegisterBankInfo.cpp",
        "AArch64A57FPLoadBalancing.cpp",
        "AArch64CodeLayoutOpt.cpp",
        "AArch64AdvSIMDScalarPass.cpp",
        "AArch64Arm64ECCallLowering.cpp",
        "AArch64AsmPrinter.cpp",
        "AArch64BranchTargets.cpp",
        "AArch64CallingConvention.cpp",
        "AArch64CleanupLocalDynamicTLSPass.cpp",
        "AArch64CollectLOH.cpp",
        "AArch64CondBrTuning.cpp",
        "AArch64ConditionalCompares.cpp",
        "AArch64DeadRegisterDefinitionsPass.cpp",
        "AArch64ExpandImm.cpp",
        "AArch64ExpandPseudoInsts.cpp",
        "AArch64FalkorHWPFFix.cpp",
        "AArch64FastISel.cpp",
        "AArch64A53Fix835769.cpp",
        "AArch64FrameLowering.cpp",
        "AArch64CompressJumpTables.cpp",
        "AArch64ConditionOptimizer.cpp",
        "AArch64RedundantCopyElimination.cpp",
        "AArch64RedundantCondBranchPass.cpp",
        "AArch64ISelDAGToDAG.cpp",
        "AArch64ISelLowering.cpp",
        "AArch64InstrInfo.cpp",
        "AArch64LoadStoreOptimizer.cpp",
        "AArch64LowerHomogeneousPrologEpilog.cpp",
        "AArch64MachineFunctionInfo.cpp",
        "AArch64MachineScheduler.cpp",
        "AArch64MacroFusion.cpp",
        "AArch64MIPeepholeOpt.cpp",
        "AArch64MCInstLower.cpp",
        "AArch64PerfectShuffle.cpp",
        "AArch64PointerAuth.cpp",
        "AArch64PostCoalescerPass.cpp",
        "AArch64PromoteConstant.cpp",
        "AArch64PBQPRegAlloc.cpp",
        "AArch64RegisterInfo.cpp",
        "AArch64SMEAttributes.cpp",
        "AArch64SLSHardening.cpp",
        "AArch64SelectionDAGInfo.cpp",
        "AArch64SpeculationHardening.cpp",
        "AArch64StackTagging.cpp",
        "AArch64StackTaggingPreRA.cpp",
        "AArch64StorePairSuppress.cpp",
        "AArch64Subtarget.cpp",
        "AArch64TargetMachine.cpp",
        "AArch64TargetObjectFile.cpp",
        "AArch64TargetTransformInfo.cpp",
        "SMEPeepholeOpt.cpp",
        "SVEIntrinsicOpts.cpp",
        "SVEShuffleOpts.cpp",
        "MachineSMEABIPass.cpp",
        "AArch64SRLTDefineSuperRegs.cpp",
        "AArch64SIMDInstrOpt.cpp",
        "AArch64PrologueEpilogue.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "AArch64GenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "AArch64GenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "AArch64GenAsmWriter1", .td_args = &.{ "-gen-asm-writer", "-asmwriternum=1" } },
        .{ .name = "AArch64GenCallingConv", .td_args = &.{"-gen-callingconv"} },
        .{ .name = "AArch64GenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "AArch64GenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "AArch64GenFastISel", .td_args = &.{"-gen-fast-isel"} },
        .{ .name = "AArch64GenGlobalISel", .td_args = &.{ "-gen-global-isel", "-gisel-extended-llt" } },
        .{
            .name = "AArch64GenO0PreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=AArch64O0PreLegalizerCombiner" },
        },
        .{
            .name = "AArch64GenPreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=AArch64PreLegalizerCombiner" },
        },
        .{
            .name = "AArch64GenPostLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=AArch64PostLegalizerCombiner" },
        },
        .{
            .name = "AArch64GenPostLegalizeGILowering",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=AArch64PostLegalizerLowering" },
        },
        .{ .name = "AArch64GenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "AArch64GenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "AArch64GenMCPseudoLowering", .td_args = &.{"-gen-pseudo-lowering"} },
        .{ .name = "AArch64GenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{ .name = "AArch64GenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "AArch64GenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
        .{ .name = "AArch64GenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "AArch64GenSystemOperands", .td_args = &.{"-gen-searchable-tables"} },
        .{ .name = "AArch64GenExegesis", .td_args = &.{"-gen-exegesis"} },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"AArch64AsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{
        "AArch64Disassembler.cpp",
        "AArch64ExternalSymbolizer.cpp",
    };

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "AArch64AsmBackend.cpp",
        "AArch64ELFObjectWriter.cpp",
        "AArch64ELFStreamer.cpp",
        "AArch64InstPrinter.cpp",
        "AArch64MCAsmInfo.cpp",
        "AArch64MCCodeEmitter.cpp",
        "AArch64MCExpr.cpp",
        "AArch64MCLFIRewriter.cpp",
        "AArch64MCTargetDesc.cpp",
        "AArch64MachObjectWriter.cpp",
        "AArch64TargetStreamer.cpp",
        "AArch64WinCOFFObjectWriter.cpp",
        "AArch64WinCOFFStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"AArch64TargetInfo.cpp"};

    pub const utils_root = backend_root ++ "Utils";
    pub const utils_sources = [_][]const u8{
        "AArch64BaseInfo.cpp",
    };
};

pub const X86 = struct {
    pub const backend_root = root ++ "X86/";
    pub const td_filepath = backend_root ++ "X86.td";
    pub const backend_sources = [_][]const u8{
        "X86ArgumentStackSlotRebase.cpp",
        "X86AsmPrinter.cpp",
        "X86AvoidTrailingCall.cpp",
        "X86CallFrameOptimization.cpp",
        "X86CallingConv.cpp",
        "X86CleanupLocalDynamicTLS.cpp",
        "X86CmovConversion.cpp",
        "X86CodeGenPassBuilder.cpp",
        "X86DomainReassignment.cpp",
        "X86GlobalBaseReg.cpp",
        "X86LowerTileCopy.cpp",
        "X86LowerAMXType.cpp",
        "X86LowerAMXIntrinsics.cpp",
        "X86TileConfig.cpp",
        "X86FastPreTileConfig.cpp",
        "X86FastTileConfig.cpp",
        "X86PreTileConfig.cpp",
        "X86ExpandPseudo.cpp",
        "X86FastISel.cpp",
        "X86FixupBWInsts.cpp",
        "X86FixupLEAs.cpp",
        "X86FixupInstTuning.cpp",
        "X86FixupVectorConstants.cpp",
        "X86AvoidStoreForwardingBlocks.cpp",
        "X86DynAllocaExpander.cpp",
        "X86FixupSetCC.cpp",
        "X86FlagsCopyLowering.cpp",
        "X86FloatingPoint.cpp",
        "X86FrameLowering.cpp",
        "X86ISelDAGToDAG.cpp",
        "X86ISelLowering.cpp",
        "X86ISelLoweringCall.cpp",
        "X86IndirectBranchTracking.cpp",
        "X86IndirectThunks.cpp",
        "X86InsertVZeroUpper.cpp",
        "X86InsertX87Wait.cpp",
        "X86InterleavedAccess.cpp",
        "X86InstCombineIntrinsic.cpp",
        "X86InstrFMA3Info.cpp",
        "X86InstrFoldTables.cpp",
        "X86InstrInfo.cpp",
        "X86CompressEVEX.cpp",
        "X86LoadValueInjectionLoadHardening.cpp",
        "X86LoadValueInjectionRetHardening.cpp",
        "X86MCInstLower.cpp",
        "X86MachineFunctionInfo.cpp",
        "X86MacroFusion.cpp",
        "X86OptimizeLEAs.cpp",
        "X86PadShortFunction.cpp",
        "X86PartialReduction.cpp",
        "X86RegisterInfo.cpp",
        "X86ReturnThunks.cpp",
        "X86SelectionDAGInfo.cpp",
        "X86ShuffleDecodeConstantPool.cpp",
        "X86SpeculativeLoadHardening.cpp",
        "X86SpeculativeExecutionSideEffectSuppression.cpp",
        "X86Subtarget.cpp",
        "X86SuppressAPXForReloc.cpp",
        "X86TargetMachine.cpp",
        "X86TargetObjectFile.cpp",
        "X86TargetTransformInfo.cpp",
        "X86WinEHState.cpp",
        "X86WinEHUnwindV2.cpp",
        "X86WinEHUnwindV3.cpp",
        "GISel/X86CallLowering.cpp",
        "GISel/X86InstructionSelector.cpp",
        "GISel/X86LegalizerInfo.cpp",
        "GISel/X86PostLegalizerCombiner.cpp",
        "GISel/X86PreLegalizerCombiner.cpp",
        "GISel/X86RegisterBankInfo.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "X86GenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "X86GenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "X86GenAsmWriter1", .td_args = &.{ "-gen-asm-writer", "-asmwriternum=1" } },
        .{ .name = "X86GenCallingConv", .td_args = &.{"-gen-callingconv"} },
        .{ .name = "X86GenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "X86GenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "X86GenInstrMapping", .td_args = &.{"-gen-x86-instr-mapping"} },
        .{ .name = "X86GenExegesis", .td_args = &.{"-gen-exegesis"} },
        .{ .name = "X86GenFastISel", .td_args = &.{"-gen-fast-isel"} },
        .{ .name = "X86GenGlobalISel", .td_args = &.{"-gen-global-isel"} },
        .{
            .name = "X86GenInstrInfo",
            .td_args = &.{ "-gen-instr-info", "-instr-info-expand-mi-operand-info=0" },
        },
        .{ .name = "X86GenMnemonicTables", .td_args = &.{ "-gen-x86-mnemonic-tables", "-asmwriternum=1" } },
        .{ .name = "X86GenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{ .name = "X86GenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "X86GenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "X86GenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
        .{
            .name = "X86GenPreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=X86PreLegalizerCombiner" },
        },
        .{
            .name = "X86GenPostLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=X86PostLegalizerCombiner" },
        },
        .{ .name = "X86GenFoldTables", .td_args = &.{ "-gen-x86-fold-tables", "-asmwriternum=1" } },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"X86AsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"X86Disassembler.cpp"};

    pub const mca_root = backend_root ++ "MCA";
    pub const mca_sources = [_][]const u8{"X86CustomBehaviour.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "X86ATTInstPrinter.cpp",
        "X86IntelInstPrinter.cpp",
        "X86InstComments.cpp",
        "X86InstPrinterCommon.cpp",
        "X86EncodingOptimization.cpp",
        "X86ShuffleDecode.cpp",
        "X86AsmBackend.cpp",
        "X86MCTargetDesc.cpp",
        "X86MCAsmInfo.cpp",
        "X86MCCodeEmitter.cpp",
        "X86MCLFIRewriter.cpp",
        "X86MachObjectWriter.cpp",
        "X86MnemonicTables.cpp",
        "X86ELFObjectWriter.cpp",
        "X86WinCOFFObjectWriter.cpp",
        "X86WinCOFFStreamer.cpp",
        "X86WinCOFFTargetStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"X86TargetInfo.cpp"};
};

pub const Arm = struct {
    pub const backend_root = root ++ "ARM/";
    pub const td_filepath = backend_root ++ "ARM.td";
    pub const backend_sources = [_][]const u8{
        "A15SDOptimizer.cpp",
        "ARMAsmPrinter.cpp",
        "ARMBaseInstrInfo.cpp",
        "ARMBaseRegisterInfo.cpp",
        "ARMBasicBlockInfo.cpp",
        "ARMBranchTargets.cpp",
        "ARMCallingConv.cpp",
        "ARMCallLowering.cpp",
        "ARMConstantIslandPass.cpp",
        "ARMConstantPoolValue.cpp",
        "ARMExpandPseudoInsts.cpp",
        "ARMFastISel.cpp",
        "ARMFixCortexA57AES1742098Pass.cpp",
        "ARMFrameLowering.cpp",
        "ARMHazardRecognizer.cpp",
        "ARMInstructionSelector.cpp",
        "ARMISelDAGToDAG.cpp",
        "ARMISelLowering.cpp",
        "ARMInstrInfo.cpp",
        "ARMLegalizerInfo.cpp",
        "ARMParallelDSP.cpp",
        "ARMLoadStoreOptimizer.cpp",
        "ARMLowOverheadLoops.cpp",
        "ARMBlockPlacement.cpp",
        "ARMMCInstLower.cpp",
        "ARMMachineFunctionInfo.cpp",
        "ARMMacroFusion.cpp",
        "ARMRegisterInfo.cpp",
        "ARMOptimizeBarriersPass.cpp",
        "ARMRegisterBankInfo.cpp",
        "ARMSelectionDAGInfo.cpp",
        "ARMSLSHardening.cpp",
        "ARMSubtarget.cpp",
        "ARMTargetMachine.cpp",
        "ARMTargetObjectFile.cpp",
        "ARMTargetTransformInfo.cpp",
        "MLxExpansionPass.cpp",
        "MVEGatherScatterLowering.cpp",
        "MVELaneInterleavingPass.cpp",
        "MVETailPredication.cpp",
        "MVEVPTBlockPass.cpp",
        "MVETPAndVPTOptimisationsPass.cpp",
        "ARMLatencyMutations.cpp",
        "Thumb1FrameLowering.cpp",
        "Thumb1InstrInfo.cpp",
        "ThumbRegisterInfo.cpp",
        "Thumb2ITBlockPass.cpp",
        "Thumb2InstrInfo.cpp",
        "Thumb2SizeReduction.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "ARMGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "ARMGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "ARMGenCallingConv", .td_args = &.{"-gen-callingconv"} },
        .{ .name = "ARMGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "ARMGenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "ARMGenFastISel", .td_args = &.{"-gen-fast-isel"} },
        .{ .name = "ARMGenGlobalISel", .td_args = &.{"-gen-global-isel"} },
        .{ .name = "ARMGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "ARMGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "ARMGenMCPseudoLowering", .td_args = &.{"-gen-pseudo-lowering"} },
        .{ .name = "ARMGenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{ .name = "ARMGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "ARMGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "ARMGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
        .{ .name = "ARMGenSystemRegister", .td_args = &.{"-gen-searchable-tables"} },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"ARMAsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"ARMDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "ARMAsmBackend.cpp",
        "ARMELFObjectWriter.cpp",
        "ARMELFStreamer.cpp",
        "ARMInstPrinter.cpp",
        "ARMMachObjectWriter.cpp",
        "ARMMachORelocationInfo.cpp",
        "ARMMCAsmInfo.cpp",
        "ARMMCCodeEmitter.cpp",
        "ARMMCTargetDesc.cpp",
        "ARMTargetStreamer.cpp",
        "ARMUnwindOpAsm.cpp",
        "ARMWinCOFFObjectWriter.cpp",
        "ARMWinCOFFStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"ARMTargetInfo.cpp"};

    pub const utils_root = backend_root ++ "Utils";
    pub const utils_sources = [_][]const u8{"ARMBaseInfo.cpp"};
};

pub const RiscV = struct {
    pub const backend_root = root ++ "RISCV/";
    pub const base_td_filepath = backend_root ++ "RISCV.td";
    pub const gisel_td_filepath = backend_root ++ "RISCVGISel.td";

    pub const backend_sources = [_][]const u8{
        "RISCVAsmPrinter.cpp",
        "RISCVCallingConv.cpp",
        "RISCVCodeGenPrepare.cpp",
        "RISCVConstantPoolValue.cpp",
        "RISCVDeadRegisterDefinitions.cpp",
        "RISCVExpandAtomicPseudoInsts.cpp",
        "RISCVExpandPseudoInsts.cpp",
        "RISCVFoldMemOffset.cpp",
        "RISCVFrameLowering.cpp",
        "RISCVGatherScatterLowering.cpp",
        "RISCVIndirectBranchTracking.cpp",
        "RISCVInsertReadWriteCSR.cpp",
        "RISCVInsertVSETVLI.cpp",
        "RISCVInsertWriteVXRM.cpp",
        "RISCVInstrInfo.cpp",
        "RISCVInterleavedAccess.cpp",
        "RISCVISelDAGToDAG.cpp",
        "RISCVISelLowering.cpp",
        "RISCVLandingPadSetup.cpp",
        "RISCVLateBranchOpt.cpp",
        "RISCVLoadStoreOptimizer.cpp",
        "RISCVMachineFunctionInfo.cpp",
        "RISCVMakeCompressible.cpp",
        "RISCVMachineScheduler.cpp",
        "RISCVMergeBaseOffset.cpp",
        "RISCVMoveMerger.cpp",
        "RISCVOptWInstrs.cpp",
        "RISCVPostRAExpandPseudoInsts.cpp",
        "RISCVPromoteConstant.cpp",
        "RISCVPushPopOptimizer.cpp",
        "RISCVQCRelaxMarking.cpp",
        "RISCVRedundantCopyElimination.cpp",
        "RISCVRegisterInfo.cpp",
        "RISCVSelectionDAGInfo.cpp",
        "RISCVSubtarget.cpp",
        "RISCVTargetMachine.cpp",
        "RISCVTargetObjectFile.cpp",
        "RISCVTargetTransformInfo.cpp",
        "RISCVVectorMaskDAGMutation.cpp",
        "RISCVVectorPeephole.cpp",
        "RISCVVLOptimizer.cpp",
        "RISCVVMV0Elimination.cpp",
        "RISCVVSETVLIInfoAnalysis.cpp",
        "RISCVZacasABIFix.cpp",
        "RISCVZilsdOptimizer.cpp",
        "GISel/RISCVCallLowering.cpp",
        "GISel/RISCVInlineAsmLowering.cpp",
        "GISel/RISCVInstructionSelector.cpp",
        "GISel/RISCVLegalizerInfo.cpp",
        "GISel/RISCVPostLegalizerCombiner.cpp",
        "GISel/RISCVO0PreLegalizerCombiner.cpp",
        "GISel/RISCVPreLegalizerCombiner.cpp",
        "GISel/RISCVRegisterBankInfo.cpp",
    };

    pub const base_actions = [_]BackendAction{
        .{ .name = "RISCVGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "RISCVGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "RISCVGenCompressInstEmitter", .td_args = &.{"-gen-compress-inst-emitter"} },
        .{ .name = "RISCVGenMacroFusion", .td_args = &.{"-gen-macro-fusion-pred"} },
        .{ .name = "RISCVGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "RISCVGenDisassemblerTables", .td_args = &.{ "-gen-disassembler", "--specialize-decoders-per-bitwidth" } },
        .{ .name = "RISCVGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "RISCVGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "RISCVGenMCPseudoLowering", .td_args = &.{"-gen-pseudo-lowering"} },
        .{ .name = "RISCVGenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{ .name = "RISCVGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "RISCVGenSearchableTables", .td_args = &.{"-gen-searchable-tables"} },
        .{ .name = "RISCVGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "RISCVGenExegesis", .td_args = &.{"-gen-exegesis"} },
        .{ .name = "RISCVGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
    };

    pub const gisel_actions = [_]BackendAction{
        .{ .name = "RISCVGenGlobalISel", .td_args = &.{"-gen-global-isel"} },
        .{
            .name = "RISCVGenO0PreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=RISCVO0PreLegalizerCombiner" },
        },
        .{
            .name = "RISCVGenPreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=RISCVPreLegalizerCombiner" },
        },
        .{
            .name = "RISCVGenPostLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=RISCVPostLegalizerCombiner" },
        },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"RISCVAsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"RISCVDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "RISCVAsmBackend.cpp",
        "RISCVBaseInfo.cpp",
        "RISCVELFObjectWriter.cpp",
        "RISCVInstPrinter.cpp",
        "RISCVMCAsmInfo.cpp",
        "RISCVMCCodeEmitter.cpp",
        "RISCVMCExpr.cpp",
        "RISCVMCObjectFileInfo.cpp",
        "RISCVMCTargetDesc.cpp",
        "RISCVMachObjectWriter.cpp",
        "RISCVMatInt.cpp",
        "RISCVTargetStreamer.cpp",
        "RISCVELFStreamer.cpp",
    };

    pub const mca_root = backend_root ++ "MCA";
    pub const mca_sources = [_][]const u8{"RISCVCustomBehaviour.cpp"};

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"RISCVTargetInfo.cpp"};
};

pub const WebAssembly = struct {
    pub const backend_root = root ++ "WebAssembly/";
    pub const td_filepath = backend_root ++ "WebAssembly.td";
    pub const backend_sources = [_][]const u8{
        "WebAssemblyAddMissingPrototypes.cpp",
        "WebAssemblyArgumentMove.cpp",
        "WebAssemblyAsmPrinter.cpp",
        "WebAssemblyCFGStackify.cpp",
        "WebAssemblyCleanCodeAfterTrap.cpp",
        "WebAssemblyCFGSort.cpp",
        "WebAssemblyDebugFixup.cpp",
        "WebAssemblyDebugValueManager.cpp",
        "WebAssemblyLateEHPrepare.cpp",
        "WebAssemblyExceptionInfo.cpp",
        "WebAssemblyExplicitLocals.cpp",
        "WebAssemblyFastISel.cpp",
        "WebAssemblyFixBrTableDefaults.cpp",
        "WebAssemblyFixIrreducibleControlFlow.cpp",
        "WebAssemblyFixFunctionBitcasts.cpp",
        "WebAssemblyFrameLowering.cpp",
        "WebAssemblyISelDAGToDAG.cpp",
        "WebAssemblyISelLowering.cpp",
        "WebAssemblyInstrInfo.cpp",
        "WebAssemblyLowerBrUnless.cpp",
        "WebAssemblyLowerEmscriptenEHSjLj.cpp",
        "WebAssemblyMachineFunctionInfo.cpp",
        "WebAssemblyMCInstLower.cpp",
        "WebAssemblyMCLowerPrePass.cpp",
        "WebAssemblyNullifyDebugValueLists.cpp",
        "WebAssemblyOptimizeLiveIntervals.cpp",
        "WebAssemblyOptimizeReturned.cpp",
        "WebAssemblyPeephole.cpp",
        "WebAssemblyReduceToAnyAllTrue.cpp",
        "WebAssemblyRefTypeMem2Local.cpp",
        "WebAssemblyRegisterInfo.cpp",
        "WebAssemblyRegColoring.cpp",
        "WebAssemblyRegNumbering.cpp",
        "WebAssemblyRegStackify.cpp",
        "WebAssemblyReplacePhysRegs.cpp",
        "WebAssemblyRuntimeLibcallSignatures.cpp",
        "WebAssemblySelectionDAGInfo.cpp",
        "WebAssemblySetP2AlignOperands.cpp",
        "WebAssemblySortRegion.cpp",
        "WebAssemblyMemIntrinsicResults.cpp",
        "WebAssemblySubtarget.cpp",
        "WebAssemblyTargetMachine.cpp",
        "WebAssemblyTargetObjectFile.cpp",
        "WebAssemblyTargetTransformInfo.cpp",
        "WebAssemblyUtilities.cpp",
        "GISel/WebAssemblyCallLowering.cpp",
        "GISel/WebAssemblyInstructionSelector.cpp",
        "GISel/WebAssemblyPostLegalizerCombiner.cpp",
        "GISel/WebAssemblyPreLegalizerCombiner.cpp",
        "GISel/WebAssemblyLegalizerInfo.cpp",
        "GISel/WebAssemblyRegisterBankInfo.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "WebAssemblyGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "WebAssemblyGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "WebAssemblyGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "WebAssemblyGenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "WebAssemblyGenFastISel", .td_args = &.{"-gen-fast-isel"} },
        .{ .name = "WebAssemblyGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "WebAssemblyGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "WebAssemblyGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "WebAssemblyGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "WebAssemblyGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
        .{ .name = "WebAssemblyGenGlobalISel", .td_args = &.{ "-gen-global-isel", "-gisel-extended-llt" }, .td_file = backend_root ++ "WebAssemblyGISel.td" },
        .{ .name = "WebAssemblyGenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{
            .name = "WebAssemblyGenPreLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=WebAssemblyPreLegalizerCombiner" },
            .td_file = backend_root ++ "WebAssemblyGISel.td",
        },
        .{
            .name = "WebAssemblyGenPostLegalizeGICombiner",
            .td_args = &.{ "-gen-global-isel-combiner", "-combiners=WebAssemblyPostLegalizerCombiner" },
            .td_file = backend_root ++ "WebAssemblyGISel.td",
        },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{
        "WebAssemblyAsmParser.cpp",
        "WebAssemblyAsmTypeCheck.cpp",
    };

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"WebAssemblyDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "WebAssemblyAsmBackend.cpp",
        "WebAssemblyInstPrinter.cpp",
        "WebAssemblyMCAsmInfo.cpp",
        "WebAssemblyMCCodeEmitter.cpp",
        "WebAssemblyMCTargetDesc.cpp",
        "WebAssemblyMCTypeUtilities.cpp",
        "WebAssemblyTargetStreamer.cpp",
        "WebAssemblyWasmObjectWriter.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"WebAssemblyTargetInfo.cpp"};

    pub const utils_root = backend_root ++ "Utils";
    pub const utils_sources = [_][]const u8{"WebAssemblyTypeUtilities.cpp"};
};

pub const Xtensa = struct {
    pub const backend_root = root ++ "Xtensa/";
    pub const td_filepath = backend_root ++ "Xtensa.td";
    pub const backend_sources = [_][]const u8{
        "XtensaAsmPrinter.cpp",
        "XtensaConstantPoolValue.cpp",
        "XtensaFrameLowering.cpp",
        "XtensaInstrInfo.cpp",
        "XtensaISelDAGToDAG.cpp",
        "XtensaISelLowering.cpp",
        "XtensaRegisterInfo.cpp",
        "XtensaSelectionDAGInfo.cpp",
        "XtensaSubtarget.cpp",
        "XtensaTargetMachine.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "XtensaGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "XtensaGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "XtensaGenCallingConv", .td_args = &.{"-gen-callingconv"} },
        .{ .name = "XtensaGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "XtensaGenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "XtensaGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "XtensaGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "XtensaGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "XtensaGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "XtensaGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"XtensaAsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"XtensaDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "XtensaAsmBackend.cpp",
        "XtensaELFObjectWriter.cpp",
        "XtensaInstPrinter.cpp",
        "XtensaMCAsmInfo.cpp",
        "XtensaMCCodeEmitter.cpp",
        "XtensaMCTargetDesc.cpp",
        "XtensaTargetStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"XtensaTargetInfo.cpp"};
};

pub const PowerPC = struct {
    pub const backend_root = root ++ "PowerPC/";
    pub const td_filepath = backend_root ++ "PPC.td";
    pub const backend_sources = [_][]const u8{
        "GISel/PPCInstructionSelector.cpp",
        "PPCBoolRetToInt.cpp",
        "PPCAsmPrinter.cpp",
        "PPCBranchSelector.cpp",
        "PPCBranchCoalescing.cpp",
        "PPCCallingConv.cpp",
        "PPCCTRLoops.cpp",
        "PPCCTRLoopsVerify.cpp",
        "PPCExpandAtomicPseudoInsts.cpp",
        "PPCHazardRecognizers.cpp",
        "PPCInstrInfo.cpp",
        "PPCISelDAGToDAG.cpp",
        "PPCISelLowering.cpp",
        "PPCEarlyReturn.cpp",
        "PPCFastISel.cpp",
        "PPCFrameLowering.cpp",
        "PPCLoopInstrFormPrep.cpp",
        "PPCMCInstLower.cpp",
        "PPCMachineFunctionInfo.cpp",
        "PPCMachineScheduler.cpp",
        "PPCMacroFusion.cpp",
        "PPCMIPeephole.cpp",
        "PPCPrepareIFuncsOnAIX.cpp",
        "PPCRegisterInfo.cpp",
        "PPCSelectionDAGInfo.cpp",
        "PPCSubtarget.cpp",
        "PPCTargetMachine.cpp",
        "PPCTargetObjectFile.cpp",
        "PPCTargetTransformInfo.cpp",
        "PPCTOCRegDeps.cpp",
        "PPCTLSDynamicCall.cpp",
        "PPCVSXWACCCopy.cpp",
        "PPCReduceCRLogicals.cpp",
        "PPCVSXFMAMutate.cpp",
        "PPCVSXSwapRemoval.cpp",
        "PPCPreEmitPeephole.cpp",
        "PPCLowerMASSVEntries.cpp",
        "PPCGenScalarMASSEntries.cpp",
        "GISel/PPCCallLowering.cpp",
        "GISel/PPCRegisterBankInfo.cpp",
        "GISel/PPCLegalizerInfo.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "PPCGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "PPCGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "PPCGenCallingConv", .td_args = &.{"-gen-callingconv"} },
        .{ .name = "PPCGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "PPCGenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "PPCGenFastISel", .td_args = &.{"-gen-fast-isel"} },
        .{ .name = "PPCGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "PPCGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "PPCGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "PPCGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "PPCGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
        .{ .name = "PPCGenExegesis", .td_args = &.{"-gen-exegesis"} },
        .{ .name = "PPCGenRegisterBank", .td_args = &.{"-gen-register-bank"} },
        .{ .name = "PPCGenGlobalISel", .td_args = &.{"-gen-global-isel"} },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"PPCAsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"PPCDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "PPCAsmBackend.cpp",
        "PPCInstPrinter.cpp",
        "PPCMCTargetDesc.cpp",
        "PPCMCAsmInfo.cpp",
        "PPCMCCodeEmitter.cpp",
        "PPCPredicates.cpp",
        "PPCELFObjectWriter.cpp",
        "PPCXCOFFObjectWriter.cpp",
        "PPCELFStreamer.cpp",
        "PPCXCOFFStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"PowerPCTargetInfo.cpp"};
};

pub const LoongArch = struct {
    pub const backend_root = root ++ "LoongArch/";
    pub const td_filepath = backend_root ++ "LoongArch.td";
    pub const backend_sources = [_][]const u8{
        "LoongArchAsmPrinter.cpp",
        "LoongArchDeadRegisterDefinitions.cpp",
        "LoongArchExpandAtomicPseudoInsts.cpp",
        "LoongArchExpandPseudoInsts.cpp",
        "LoongArchFrameLowering.cpp",
        "LoongArchInstrInfo.cpp",
        "LoongArchISelDAGToDAG.cpp",
        "LoongArchISelLowering.cpp",
        "LoongArchMCInstLower.cpp",
        "LoongArchMergeBaseOffset.cpp",
        "LoongArchOptWInstrs.cpp",
        "LoongArchRegisterInfo.cpp",
        "LoongArchSelectionDAGInfo.cpp",
        "LoongArchSubtarget.cpp",
        "LoongArchTargetMachine.cpp",
        "LoongArchTargetObjectFile.cpp",
        "LoongArchTargetTransformInfo.cpp",
    };

    pub const actions = [_]BackendAction{
        .{ .name = "LoongArchGenAsmMatcher", .td_args = &.{"-gen-asm-matcher"} },
        .{ .name = "LoongArchGenAsmWriter", .td_args = &.{"-gen-asm-writer"} },
        .{ .name = "LoongArchGenDAGISel", .td_args = &.{"-gen-dag-isel"} },
        .{ .name = "LoongArchGenDisassemblerTables", .td_args = &.{"-gen-disassembler"} },
        .{ .name = "LoongArchGenInstrInfo", .td_args = &.{"-gen-instr-info"} },
        .{ .name = "LoongArchGenMCPseudoLowering", .td_args = &.{"-gen-pseudo-lowering"} },
        .{ .name = "LoongArchGenMCCodeEmitter", .td_args = &.{"-gen-emitter"} },
        .{ .name = "LoongArchGenRegisterInfo", .td_args = &.{"-gen-register-info"} },
        .{ .name = "LoongArchGenSubtargetInfo", .td_args = &.{"-gen-subtarget"} },
        .{ .name = "LoongArchGenSDNodeInfo", .td_args = &.{"-gen-sd-node-info"} },
    };

    pub const asm_parser_root = backend_root ++ "AsmParser";
    pub const asm_parser_sources = [_][]const u8{"LoongArchAsmParser.cpp"};

    pub const disassembler_root = backend_root ++ "Disassembler";
    pub const disassembler_sources = [_][]const u8{"LoongArchDisassembler.cpp"};

    pub const desc_root = backend_root ++ "MCTargetDesc";
    pub const desc_sources = [_][]const u8{
        "LoongArchAsmBackend.cpp",
        "LoongArchBaseInfo.cpp",
        "LoongArchELFObjectWriter.cpp",
        "LoongArchELFStreamer.cpp",
        "LoongArchInstPrinter.cpp",
        "LoongArchMCAsmInfo.cpp",
        "LoongArchMCCodeEmitter.cpp",
        "LoongArchMCTargetDesc.cpp",
        "LoongArchMatInt.cpp",
        "LoongArchTargetStreamer.cpp",
    };

    pub const info_root = backend_root ++ "TargetInfo";
    pub const info_sources = [_][]const u8{"LoongArchTargetInfo.cpp"};
};
