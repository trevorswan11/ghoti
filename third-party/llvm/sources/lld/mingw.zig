//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/MinGW/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "lld/MinGW/";
pub const sources = [_][]const u8{"Driver.cpp"};

pub const synthesize_options: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "MinGWOptionsTableGen",
        .td_file = root ++ "Options.td",
        .instruction = .{ .action = "-gen-opt-parser-defs" },
    },
    .virtual_path = "Options.inc",
};
