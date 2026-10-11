//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/APINotes/CMakeLists.txt
pub const root = "clang/lib/APINotes";
pub const sources = [_][]const u8{
    "APINotesManager.cpp",
    "APINotesReader.cpp",
    "APINotesTypes.cpp",
    "APINotesWriter.cpp",
    "APINotesYAMLCompiler.cpp",
};
