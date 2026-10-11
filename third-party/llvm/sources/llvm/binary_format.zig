//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/BinaryFormat/CMakeLists.txt
pub const root = "llvm/lib/BinaryFormat";
pub const sources = [_][]const u8{
    "AMDGPUMetadataVerifier.cpp",
    "COFF.cpp",
    "Dwarf.cpp",
    "DXContainer.cpp",
    "ELF.cpp",
    "MachO.cpp",
    "Magic.cpp",
    "MsgPackDocument.cpp",
    "MsgPackDocumentYAML.cpp",
    "MsgPackReader.cpp",
    "MsgPackWriter.cpp",
    "SFrame.cpp",
    "Wasm.cpp",
    "XCOFF.cpp",
};
