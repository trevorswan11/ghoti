//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ExtractAPI/CMakeLists.txt
pub const root = "clang/lib/ExtractAPI";
pub const sources = [_][]const u8{
    "API.cpp",
    "APIIgnoresList.cpp",
    "ExtractAPIConsumer.cpp",
    "DeclarationFragments.cpp",
    "Serialization/SymbolGraphSerializer.cpp",
    "TypedefUnderlyingTypeResolver.cpp",
};
