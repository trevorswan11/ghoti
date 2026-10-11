//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/Frontend
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "llvm/lib/Frontend/";

const include_root = "llvm/include/llvm/Frontend/";
pub const synthesize_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "OMP",
            .td_file = include_root ++ "OpenMP/OMP.td",
            .instruction = .{ .action = "-gen-directive-impl" },
        },
        .virtual_path = "llvm/Frontend/OpenMP/OMP.inc",
    },
    .{
        .gen_conf = .{
            .name = "OMP.h",
            .td_file = include_root ++ "OpenMP/OMP.td",
            .instruction = .{ .action = "-gen-directive-decl" },
        },
        .virtual_path = "llvm/Frontend/OpenMP/OMP.h.inc",
    },
    .{
        .gen_conf = .{
            .name = "ACC",
            .td_file = include_root ++ "OpenACC/ACC.td",
            .instruction = .{ .action = "-gen-directive-impl" },
        },
        .virtual_path = "llvm/Frontend/OpenACC/ACC.inc",
    },
    .{
        .gen_conf = .{
            .name = "ACC.h",
            .td_file = include_root ++ "OpenACC/ACC.td",
            .instruction = .{ .action = "-gen-directive-decl" },
        },
        .virtual_path = "llvm/Frontend/OpenACC/ACC.h.inc",
    },
};

pub const atomic_root = root ++ "Atomic";
pub const atomic_sources = [_][]const u8{"Atomic.cpp"};

pub const directive_root = root ++ "Directive";
pub const directive_sources = [_][]const u8{"Spelling.cpp"};

pub const driver_root = root ++ "Driver";
pub const driver_sources = [_][]const u8{"CodeGenOptions.cpp"};

pub const hlsl_root = root ++ "HLSL";
pub const hlsl_sources = [_][]const u8{
    "CBuffer.cpp",
    "HLSLBinding.cpp",
    "HLSLResource.cpp",
    "HLSLRootSignature.cpp",
    "RootSignatureMetadata.cpp",
    "RootSignatureValidations.cpp",
};

pub const offloading_root = root ++ "Offloading";
pub const offloading_sources = [_][]const u8{
    "Utility.cpp",
    "OffloadWrapper.cpp",
    "PropertySet.cpp",
};

pub const open_mp_root = root ++ "OpenMP";
pub const open_mp_sources = [_][]const u8{
    "OMP.cpp",
    "OMPContext.cpp",
    "OMPIRBuilder.cpp",
    "DirectiveNameParser.cpp",
};

pub const open_acc_root = root ++ "OpenACC";
pub const open_acc_sources = [_][]const u8{"ACC.cpp"};
