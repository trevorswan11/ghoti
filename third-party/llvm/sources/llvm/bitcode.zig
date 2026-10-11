//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/Bitcode
const root = "llvm/lib/Bitcode/";

pub const reader_root = root ++ "Reader";
pub const reader_sources = [_][]const u8{
    "BitcodeAnalyzer.cpp",
    "BitReader.cpp",
    "BitcodeReader.cpp",
    "MetadataLoader.cpp",
    "ValueList.cpp",
};

pub const writer_root = root ++ "Writer";
pub const writer_sources = [_][]const u8{
    "BitWriter.cpp",
    "BitcodeWriter.cpp",
    "BitcodeWriterPass.cpp",
    "ValueEnumerator.cpp",
};
