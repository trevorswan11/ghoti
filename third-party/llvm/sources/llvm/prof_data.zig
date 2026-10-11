//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/ProfileData/CMakeLists.txt
pub const prof_data_root = "llvm/lib/ProfileData/";
pub const prof_data_sources = [_][]const u8{
    "DataAccessProf.cpp",
    "ETMTraceDecoder.cpp",
    "GCOV.cpp",
    "IndexedMemProfData.cpp",
    "InstrProf.cpp",
    "InstrProfCorrelator.cpp",
    "InstrProfReader.cpp",
    "InstrProfWriter.cpp",
    "ItaniumManglingCanonicalizer.cpp",
    "MemProf.cpp",
    "MemProfCommon.cpp",
    "MemProfReader.cpp",
    "MemProfRadixTree.cpp",
    "MemProfSummary.cpp",
    "MemProfSummaryBuilder.cpp",
    "PGOCtxProfReader.cpp",
    "PGOCtxProfWriter.cpp",
    "ProfileSummaryBuilder.cpp",
    "SampleProf.cpp",
    "SampleProfReader.cpp",
    "SampleProfWriter.cpp",
    "SymbolRemappingReader.cpp",
};

pub const coverage_root = prof_data_root ++ "Coverage";
pub const coverage_sources = [_][]const u8{
    "CoverageMapping.cpp",
    "CoverageMappingWriter.cpp",
    "CoverageMappingReader.cpp",
};
