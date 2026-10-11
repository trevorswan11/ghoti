//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/TextAPI/CMakeLists.txt
pub const root = "llvm/lib/TextAPI/";
pub const sources = [_][]const u8{
    "Architecture.cpp",
    "ArchitectureSet.cpp",
    "InterfaceFile.cpp",
    "TextStubV5.cpp",
    "PackedVersion.cpp",
    "Platform.cpp",
    "RecordsSlice.cpp",
    "RecordVisitor.cpp",
    "Symbol.cpp",
    "SymbolSet.cpp",
    "Target.cpp",
    "TextAPIError.cpp",
    "TextStub.cpp",
    "TextStubCommon.cpp",
    "Utils.cpp",
};
