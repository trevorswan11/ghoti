//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/ExecutionEngine/CMakeLists.txt
pub const root = "llvm/lib/ExecutionEngine/";
pub const base_sources = [_][]const u8{
    "ExecutionEngine.cpp",
    "ExecutionEngineBindings.cpp",
    "GDBRegistrationListener.cpp",
    "SectionMemoryManager.cpp",
    "TargetSelect.cpp",
};

pub const interpreter_root = root ++ "Interpreter";
pub const interpreter_sources = [_][]const u8{
    "Execution.cpp",
    "ExternalFunctions.cpp",
    "Interpreter.cpp",
};

pub const jit_link_root = root ++ "JITLink/";
pub const jit_link_sources = [_][]const u8{
    "CompactUnwindSupport.cpp",
    "DWARFRecordSectionSplitter.cpp",
    "EHFrameSupport.cpp",
    "JITLink.cpp",
    "JITLinkGeneric.cpp",
    "JITLinkMemoryManager.cpp",
    "MachO.cpp",
    "MachO_arm64.cpp",
    "MachO_x86_64.cpp",
    "MachOLinkGraphBuilder.cpp",
    "ELF.cpp",
    "ELFLinkGraphBuilder.cpp",
    "ELF_aarch32.cpp",
    "ELF_aarch64.cpp",
    "ELF_hexagon.cpp",
    "ELF_loongarch.cpp",
    "ELF_ppc64.cpp",
    "ELF_riscv.cpp",
    "ELF_systemz.cpp",
    "ELF_x86.cpp",
    "ELF_x86_64.cpp",
    "COFF.cpp",
    "COFFDirectiveParser.cpp",
    "COFFLinkGraphBuilder.cpp",
    "COFF_x86_64.cpp",
    "XCOFF.cpp",
    "XCOFF_ppc64.cpp",
    "XCOFFLinkGraphBuilder.cpp",
    "aarch32.cpp",
    "aarch64.cpp",
    "hexagon.cpp",
    "loongarch.cpp",
    "ppc64.cpp",
    "riscv.cpp",
    "systemz.cpp",
    "x86.cpp",
    "x86_64.cpp",
};

pub const mc_jit_root = root ++ "MCJIT";
pub const mc_jit_sources = [_][]const u8{"MCJIT.cpp"};

pub const runtime_dyld_root = root ++ "RuntimeDyld";
pub const runtime_dyld_sources = [_][]const u8{
    "JITSymbol.cpp",
    "RTDyldMemoryManager.cpp",
    "RuntimeDyld.cpp",
    "RuntimeDyldChecker.cpp",
    "RuntimeDyldCOFF.cpp",
    "RuntimeDyldELF.cpp",
    "RuntimeDyldMachO.cpp",
    "Targets/RuntimeDyldELFMips.cpp",
};

pub const orc_root = root ++ "Orc/";
pub const orc_base_sources = [_][]const u8{
    "AbsoluteSymbols.cpp",
    "BacktraceTools.cpp",
    "COFF.cpp",
    "COFFVCRuntimeSupport.cpp",
    "COFFPlatform.cpp",
    "CompileOnDemandLayer.cpp",
    "CompileUtils.cpp",
    "Core.cpp",
    "DebugUtils.cpp",
    "EHFrameRegistrationPlugin.cpp",
    "EPCDynamicLibrarySearchGenerator.cpp",
    "EPCGenericDylibManager.cpp",
    "EPCGenericJITLinkMemoryManager.cpp",
    "EPCGenericRTDyldMemoryManager.cpp",
    "EPCIndirectionUtils.cpp",
    "ExecutionUtils.cpp",
    "ExecutorResolutionGenerator.cpp",
    "ObjectFileInterface.cpp",
    "IndirectionUtils.cpp",
    "InProcessEPC.cpp",
    "InProcessMemoryAccess.cpp",
    "IRCompileLayer.cpp",
    "IRTransformLayer.cpp",
    "IRPartitionLayer.cpp",
    "JITTargetMachineBuilder.cpp",
    "JITLinkReentryTrampolines.cpp",
    "LazyObjectLinkingLayer.cpp",
    "LazyReexports.cpp",
    "Layer.cpp",
    "LinkGraphLayer.cpp",
    "LinkGraphLinkingLayer.cpp",
    "LoadLinkableFile.cpp",
    "LookupAndRecordAddrs.cpp",
    "LLJIT.cpp",
    "MachO.cpp",
    "MachOPlatform.cpp",
    "MapperJITLinkMemoryManager.cpp",
    "MemoryMapper.cpp",
    "ELFNixPlatform.cpp",
    "Mangling.cpp",
    "ObjectLinkingLayer.cpp",
    "ObjectTransformLayer.cpp",
    "OrcABISupport.cpp",
    "OrcV2CBindings.cpp",
    "RTDyldObjectLinkingLayer.cpp",
    "SectCreate.cpp",
    "SelfExecutorProcessControl.cpp",
    "SimpleRemoteEPC.cpp",
    "SimpleRemoteMemoryMapper.cpp",
    "Speculation.cpp",
    "SpeculateAnalyses.cpp",
    "ExecutorProcessControl.cpp",
    "TaskDispatch.cpp",
    "ThreadSafeModule.cpp",
    "UnwindInfoRegistrationPlugin.cpp",
    "RedirectionManager.cpp",
    "JITLinkRedirectableSymbolManager.cpp",
    "ReOptimizeLayer.cpp",
};

pub const orc_debug_root = orc_root ++ "Debugging";
pub const orc_debug_sources = [_][]const u8{
    "DebugInfoSupport.cpp",
    "DebuggerSupport.cpp",
    "DebuggerSupportPlugin.cpp",
    "ELFDebugObjectPlugin.cpp",
    "LLJITUtilsCBindings.cpp",
    "PerfSupportPlugin.cpp",
    "VTuneSupportPlugin.cpp",
};

pub const orc_shared_root = orc_root ++ "Shared";
pub const orc_shared_sources = [_][]const u8{
    "AllocationActions.cpp",
    "MachOObjectFormat.cpp",
    "ObjectFormats.cpp",
    "OrcError.cpp",
    "OrcRTBridge.cpp",
    "SimpleRemoteEPCUtils.cpp",
    "SymbolStringPool.cpp",
};

pub const orc_target_process_root = orc_root ++ "TargetProcess";
pub const orc_target_process_sources = [_][]const u8{
    "ExecutorSharedMemoryMapperService.cpp",
    "DefaultHostBootstrapValues.cpp",
    "ExecutorResolver.cpp",
    "LibraryResolver.cpp",
    "JITLoaderGDB.cpp",
    "JITLoaderPerf.cpp",
    "JITLoaderVTune.cpp",
    "LibraryScanner.cpp",
    "OrcRTBootstrap.cpp",
    "RegisterEHFrames.cpp",
    "SimpleExecutorDylibManager.cpp",
    "SimpleExecutorMemoryManager.cpp",
    "SimpleRemoteEPCServer.cpp",
    "TargetExecutionUtils.cpp",
    "UnwindInfoManager.cpp",
};
