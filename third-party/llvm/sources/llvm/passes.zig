//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/Passes/CMakeLists.txt
pub const root = "llvm/lib/Passes";
pub const sources = [_][]const u8{
    "CodeGenPassBuilder.cpp",
    "PassBuilder.cpp",
    "PassBuilderBindings.cpp",
    "PassBuilderPipelines.cpp",
    "StandardInstrumentations.cpp",
};
