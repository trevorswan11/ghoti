//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Basic/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "clang/lib/Basic/";
pub const sources = [_][]const u8{
    "ASTSourceDescriptor.cpp",
    "Attributes.cpp",
    "AtomicLineLogger.cpp",
    "Builtins.cpp",
    "CLWarnings.cpp",
    "CharInfo.cpp",
    "CodeGenOptions.cpp",
    "Cuda.cpp",
    "DarwinSDKInfo.cpp",
    "Diagnostic.cpp",
    "DiagnosticIDs.cpp",
    "DiagnosticOptions.cpp",
    "ExpressionTraits.cpp",
    "FileEntry.cpp",
    "FileManager.cpp",
    "IdentifierTable.cpp",
    "LangOptions.cpp",
    "LangStandards.cpp",
    "MakeSupport.cpp",
    "Module.cpp",
    "ObjCRuntime.cpp",
    "OffloadArch.cpp",
    "OpenCLOptions.cpp",
    "OpenMPKinds.cpp",
    "OperatorPrecedence.cpp",
    "ParsedAttrInfo.cpp",
    "ProfileList.cpp",
    "NoSanitizeList.cpp",
    "SanitizerSpecialCaseList.cpp",
    "Sanitizers.cpp",
    "Sarif.cpp",
    "SimpleTypoCorrection.cpp",
    "SourceLocation.cpp",
    "SourceManager.cpp",
    "SourceMgrAdapter.cpp",
    "Stack.cpp",
    "StackExhaustionHandler.cpp",
    "TargetID.cpp",
    "TargetInfo.cpp",
    "Targets.cpp",
    "Targets/AArch64.cpp",
    "Targets/AMDGPU.cpp",
    "Targets/ARC.cpp",
    "Targets/ARM.cpp",
    "Targets/AVR.cpp",
    "Targets/BPF.cpp",
    "Targets/CSKY.cpp",
    "Targets/DirectX.cpp",
    "Targets/Hexagon.cpp",
    "Targets/Lanai.cpp",
    "Targets/LoongArch.cpp",
    "Targets/M68k.cpp",
    "Targets/MSP430.cpp",
    "Targets/Mips.cpp",
    "Targets/NVPTX.cpp",
    "Targets/OSTargets.cpp",
    "Targets/PPC.cpp",
    "Targets/RISCV.cpp",
    "Targets/SPIR.cpp",
    "Targets/Sparc.cpp",
    "Targets/SystemZ.cpp",
    "Targets/TCE.cpp",
    "Targets/VE.cpp",
    "Targets/WebAssembly.cpp",
    "Targets/X86.cpp",
    "Targets/XCore.cpp",
    "Targets/Xtensa.cpp",
    "TokenKinds.cpp",
    "TypeTraits.cpp",
    "Version.cpp",
    "Warnings.cpp",
    "XRayInstr.cpp",
    "XRayLists.cpp",
};

pub const targets_root = root ++ "Targets";
pub const include_root = "clang/include/clang/Basic/";

const diagnostics_td = include_root ++ "Diagnostic.td";

const diag_components = [_][]const u8{
    "Analysis",      "AST",        "Comment", "Common", "CrossTU",     "Driver",
    "Frontend",      "InstallAPI", "Lex",     "Parse",  "Refactoring", "Sema",
    "Serialization", "Trap",
};

pub const diag_synthesize_configs = blk: {
    var configs: [4 + 4 * diag_components.len]SynthesizeHeaderConfig = undefined;
    var idx = 0;

    // Per-component gen
    for (diag_components) |component| {
        const component_action = "-clang-component=" ++ component;
        configs[idx] = .{
            .gen_conf = .{
                .name = "ClangDiagnostic" ++ component ++ "Kinds",
                .td_file = diagnostics_td,
                .instruction = .{
                    .actions = &.{ "-gen-clang-diags-defs", component_action },
                },
            },
            .virtual_path = "clang/Basic/Diagnostic" ++ component ++ "Kinds.inc",
        };
        idx += 1;

        configs[idx] = .{
            .gen_conf = .{
                .name = "ClangDiagnostic" ++ component ++ "Enums",
                .td_file = diagnostics_td,
                .instruction = .{
                    .actions = &.{ "-gen-clang-diags-enums", component_action },
                },
            },
            .virtual_path = "clang/Basic/Diagnostic" ++ component ++ "Enums.inc",
        };
        idx += 1;

        configs[idx] = .{
            .gen_conf = .{
                .name = "ClangDiagnostic" ++ component ++ "CompatIDs",
                .td_file = diagnostics_td,
                .instruction = .{
                    .actions = &.{ "-gen-clang-diags-compat-ids", component_action },
                },
            },
            .virtual_path = "clang/Basic/Diagnostic" ++ component ++ "CompatIDs.inc",
        };
        idx += 1;

        configs[idx] = .{
            .gen_conf = .{
                .name = "ClangDiagnostic" ++ component ++ "Interface",
                .td_file = diagnostics_td,
                .instruction = .{
                    .actions = &.{ "-gen-clang-diags-iface", component_action },
                },
            },
            .virtual_path = "clang/Basic/Diagnostic" ++ component ++ "Interface.inc",
        };
        idx += 1;
    }

    // General gen
    configs[idx] = .{
        .gen_conf = .{
            .name = "ClangDiagnosticGroups",
            .td_file = diagnostics_td,
            .instruction = .{ .action = "-gen-clang-diag-groups" },
        },
        .virtual_path = "clang/Basic/DiagnosticGroups.inc",
    };
    idx += 1;

    configs[idx] = .{
        .gen_conf = .{
            .name = "ClangDiagnosticIndexName",
            .td_file = diagnostics_td,
            .instruction = .{ .action = "-gen-clang-diags-index-name" },
        },
        .virtual_path = "clang/Basic/DiagnosticIndexName.inc",
    };
    idx += 1;

    configs[idx] = .{
        .gen_conf = .{
            .name = "ClangDiagnosticStableIDs",
            .td_file = diagnostics_td,
            .instruction = .{ .action = "-gen-clang-diags-stable-ids" },
        },
        .virtual_path = "clang/Basic/DiagnosticStableIDs.inc",
    };
    idx += 1;

    configs[idx] = .{
        .gen_conf = .{
            .name = "ClangDiagnosticAllCompatIDs",
            .td_file = diagnostics_td,
            .instruction = .{ .action = "-gen-clang-diags-compat-ids" },
        },
        .virtual_path = "clang/Basic/DiagnosticAllCompatIDs.inc",
    };

    break :blk configs;
};

pub const attr_td = include_root ++ "Attr.td";

pub const attr_synthesize_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "ClangAttrList",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-list" },
        },
        .virtual_path = "clang/Basic/AttrList.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrParsedAttrList",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-parsed-attr-list" },
        },
        .virtual_path = "clang/Basic/AttrParsedAttrList.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrSubjectMatchRuleList",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-subject-match-rule-list" },
        },
        .virtual_path = "clang/Basic/AttrSubMatchRulesList.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRegularKeywordAttrInfo",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-regular-keyword-attr-info" },
        },
        .virtual_path = "clang/Basic/RegularKeywordAttrInfo.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrHasAttributeImpl",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-has-attribute-impl" },
        },
        .virtual_path = "clang/Basic/AttrHasAttributeImpl.inc",
    },
    .{
        .gen_conf = .{
            .name = "CXX11AttributeInfo",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-cxx11-attribute-info" },
        },
        .virtual_path = "clang/Basic/CXX11AttributeInfo.inc",
    },
    .{
        .gen_conf = .{
            .name = "AttributeSpellingList",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-attribute-spelling-list" },
        },
        .virtual_path = "clang/Basic/AttributeSpellingList.inc",
    },
};

pub const builtin_synthesize_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "ClangBuiltins",
            .td_file = include_root ++ "Builtins.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/Builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsAArch64",
            .td_file = include_root ++ "BuiltinsAArch64.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsAArch64.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsAMDGPU",
            .td_file = include_root ++ "BuiltinsAMDGPU.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsAMDGPU.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsBPF",
            .td_file = include_root ++ "BuiltinsBPF.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsBPF.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsDirectX",
            .td_file = include_root ++ "BuiltinsDirectX.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsDirectX.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsHexagon",
            .td_file = include_root ++ "BuiltinsHexagon.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsHexagon.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsNVPTX",
            .td_file = include_root ++ "BuiltinsNVPTX.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsNVPTX.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsRISCV",
            .td_file = include_root ++ "BuiltinsRISCV.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsRISCV.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsSPIRVCommon",
            .td_file = include_root ++ "BuiltinsSPIRVCommon.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsSPIRVCommon.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsSPIRVVK",
            .td_file = include_root ++ "BuiltinsSPIRVVK.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsSPIRVVK.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsSPIRVCL",
            .td_file = include_root ++ "BuiltinsSPIRVCL.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsSPIRVCL.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsSystemZ",
            .td_file = include_root ++ "BuiltinsSystemZ.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsSystemZ.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsX86",
            .td_file = include_root ++ "BuiltinsX86.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsX86.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsX86_64",
            .td_file = include_root ++ "BuiltinsX86_64.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsX86_64.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinsZOS",
            .td_file = include_root ++ "BuiltinsZOS.td",
            .instruction = .{ .action = "-gen-clang-builtins" },
        },
        .virtual_path = "clang/Basic/BuiltinsZOS.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangBuiltinTemplates",
            .td_file = include_root ++ "BuiltinTemplates.td",
            .instruction = .{ .action = "-gen-clang-builtin-templates" },
        },
        .virtual_path = "clang/Basic/BuiltinTemplates.inc",
    },
};

pub const arch_synthesize_configs = [_]SynthesizeHeaderConfig{
    // ARM NEON / FP16
    .{
        .gen_conf = .{
            .name = "ClangARMNeon",
            .td_file = include_root ++ "arm_neon.td",
            .instruction = .{ .action = "-gen-arm-neon-sema" },
        },
        .virtual_path = "clang/Basic/arm_neon.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMFP16",
            .td_file = include_root ++ "arm_fp16.td",
            .instruction = .{ .action = "-gen-arm-neon-sema" },
        },
        .virtual_path = "clang/Basic/arm_fp16.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMImmChecks",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-immcheck-types" },
        },
        .virtual_path = "clang/Basic/arm_immcheck_types.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSveBuiltins",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-sve-builtins" },
        },
        .virtual_path = "clang/Basic/arm_sve_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSveBuiltinCG",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-sve-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/arm_sve_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSveTypeFlags",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-sve-typeflags" },
        },
        .virtual_path = "clang/Basic/arm_sve_typeflags.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSveSemaRangeChecks",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-sve-sema-rangechecks" },
        },
        .virtual_path = "clang/Basic/arm_sve_sema_rangechecks.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSveStreamingAttrs",
            .td_file = include_root ++ "arm_sve.td",
            .instruction = .{ .action = "-gen-arm-sve-streaming-attrs" },
        },
        .virtual_path = "clang/Basic/arm_sve_streaming_attrs.inc",
    },

    // ARM MVE
    .{
        .gen_conf = .{
            .name = "ClangARMMveBuiltinsDef",
            .td_file = include_root ++ "arm_mve.td",
            .instruction = .{ .action = "-gen-arm-mve-builtin-def" },
        },
        .virtual_path = "clang/Basic/arm_mve_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMMveBuiltinCG",
            .td_file = include_root ++ "arm_mve.td",
            .instruction = .{ .action = "-gen-arm-mve-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/arm_mve_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMMveBuiltinSema",
            .td_file = include_root ++ "arm_mve.td",
            .instruction = .{ .action = "-gen-arm-mve-builtin-sema" },
        },
        .virtual_path = "clang/Basic/arm_mve_builtin_sema.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMMveBuiltinAliases",
            .td_file = include_root ++ "arm_mve.td",
            .instruction = .{ .action = "-gen-arm-mve-builtin-aliases" },
        },
        .virtual_path = "clang/Basic/arm_mve_builtin_aliases.inc",
    },

    // ARM SME
    .{
        .gen_conf = .{
            .name = "ClangARMSmeBuiltins",
            .td_file = include_root ++ "arm_sme.td",
            .instruction = .{ .action = "-gen-arm-sme-builtins" },
        },
        .virtual_path = "clang/Basic/arm_sme_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSmeBuiltinCG",
            .td_file = include_root ++ "arm_sme.td",
            .instruction = .{ .action = "-gen-arm-sme-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/arm_sme_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSmeSemaRangeChecks",
            .td_file = include_root ++ "arm_sme.td",
            .instruction = .{ .action = "-gen-arm-sme-sema-rangechecks" },
        },
        .virtual_path = "clang/Basic/arm_sme_sema_rangechecks.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSmeStreamingAttrs",
            .td_file = include_root ++ "arm_sme.td",
            .instruction = .{ .action = "-gen-arm-sme-streaming-attrs" },
        },
        .virtual_path = "clang/Basic/arm_sme_streaming_attrs.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMSmeBuiltinsZAState",
            .td_file = include_root ++ "arm_sme.td",
            .instruction = .{ .action = "-gen-arm-sme-builtin-za-state" },
        },
        .virtual_path = "clang/Basic/arm_sme_builtins_za_state.inc",
    },

    // ARM CDE
    .{
        .gen_conf = .{
            .name = "ClangARMCdeBuiltinsDef",
            .td_file = include_root ++ "arm_cde.td",
            .instruction = .{ .action = "-gen-arm-cde-builtin-def" },
        },
        .virtual_path = "clang/Basic/arm_cde_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMCdeBuiltinCG",
            .td_file = include_root ++ "arm_cde.td",
            .instruction = .{ .action = "-gen-arm-cde-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/arm_cde_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMCdeBuiltinSema",
            .td_file = include_root ++ "arm_cde.td",
            .instruction = .{ .action = "-gen-arm-cde-builtin-sema" },
        },
        .virtual_path = "clang/Basic/arm_cde_builtin_sema.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangARMCdeBuiltinAliases",
            .td_file = include_root ++ "arm_cde.td",
            .instruction = .{ .action = "-gen-arm-cde-builtin-aliases" },
        },
        .virtual_path = "clang/Basic/arm_cde_builtin_aliases.inc",
    },

    // RISC-V Vector
    .{
        .gen_conf = .{
            .name = "ClangRISCVVectorBuiltins",
            .td_file = include_root ++ "riscv_vector.td",
            .instruction = .{ .action = "-gen-riscv-vector-builtins" },
        },
        .virtual_path = "clang/Basic/riscv_vector_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVVectorBuiltinCG",
            .td_file = include_root ++ "riscv_vector.td",
            .instruction = .{ .action = "-gen-riscv-vector-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/riscv_vector_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVVectorBuiltinSema",
            .td_file = include_root ++ "riscv_vector.td",
            .instruction = .{ .action = "-gen-riscv-vector-builtin-sema" },
        },
        .virtual_path = "clang/Basic/riscv_vector_builtin_sema.inc",
    },

    // RISC-V SiFive Vector
    .{
        .gen_conf = .{
            .name = "ClangRISCVSiFiveVectorBuiltins",
            .td_file = include_root ++ "riscv_sifive_vector.td",
            .instruction = .{ .action = "-gen-riscv-sifive-vector-builtins" },
        },
        .virtual_path = "clang/Basic/riscv_sifive_vector_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVSiFiveVectorBuiltinCG",
            .td_file = include_root ++ "riscv_sifive_vector.td",
            .instruction = .{ .action = "-gen-riscv-sifive-vector-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/riscv_sifive_vector_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVSiFiveVectorBuiltinSema",
            .td_file = include_root ++ "riscv_sifive_vector.td",
            .instruction = .{ .action = "-gen-riscv-sifive-vector-builtin-sema" },
        },
        .virtual_path = "clang/Basic/riscv_sifive_vector_builtin_sema.inc",
    },

    // RISC-V Andes Vector
    .{
        .gen_conf = .{
            .name = "ClangRISCVAndesVectorBuiltins",
            .td_file = include_root ++ "riscv_andes_vector.td",
            .instruction = .{ .action = "-gen-riscv-andes-vector-builtins" },
        },
        .virtual_path = "clang/Basic/riscv_andes_vector_builtins.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVAndesVectorBuiltinCG",
            .td_file = include_root ++ "riscv_andes_vector.td",
            .instruction = .{ .action = "-gen-riscv-andes-vector-builtin-codegen" },
        },
        .virtual_path = "clang/Basic/riscv_andes_vector_builtin_cg.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangRISCVAndesVectorBuiltinSema",
            .td_file = include_root ++ "riscv_andes_vector.td",
            .instruction = .{ .action = "-gen-riscv-andes-vector-builtin-sema" },
        },
        .virtual_path = "clang/Basic/riscv_andes_vector_builtin_sema.inc",
    },
};
