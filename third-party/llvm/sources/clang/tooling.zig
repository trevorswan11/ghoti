//! https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/clang/lib/Tooling
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

pub const root = "clang/lib/Tooling/";
pub const sources = [_][]const u8{
    "AllTUsExecution.cpp",
    "ArgumentsAdjusters.cpp",
    "CommonOptionsParser.cpp",
    "CompilationDatabase.cpp",
    "DependencyScanningTool.cpp",
    "Execution.cpp",
    "ExpandResponseFilesCompilationDatabase.cpp",
    "FileMatchTrie.cpp",
    "FixIt.cpp",
    "GuessTargetAndModeCompilationDatabase.cpp",
    "InterpolatingCompilationDatabase.cpp",
    "JSONCompilationDatabase.cpp",
    "LocateToolCompilationDatabase.cpp",
    "Refactoring.cpp",
    "RefactoringCallbacks.cpp",
    "StandaloneExecution.cpp",
    "Tooling.cpp",
};

pub const ast_diff_root = root ++ "ASTDiff";
pub const ast_diff_sources = [_][]const u8{"ASTDiff.cpp"};

pub const core_root = root ++ "Core";
pub const core_sources = [_][]const u8{
    "Diagnostic.cpp",
    "Replacement.cpp",
};

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/DependencyScanning/CMakeLists.txt
pub const dep_scanning_root = "clang/lib/DependencyScanning";
pub const dep_scanning_sources = [_][]const u8{
    "DependencyGraph.cpp",
    "DependencyScanningFilesystem.cpp",
    "DependencyScanningService.cpp",
    "DependencyScanningWorker.cpp",
    "DependencyScanningUtils.cpp",
    "DependencyScannerImpl.cpp",
    "InProcessModuleCache.cpp",
    "ModuleDepCollector.cpp",
};

pub const inclusions_root = root ++ "Inclusions/";
pub const inclusions_sources = [_][]const u8{
    "HeaderAnalysis.cpp",
    "HeaderIncludes.cpp",
    "IncludeStyle.cpp",
};

pub const inclusions_stdlib_root = inclusions_root ++ "Stdlib";
pub const inclusions_stdlib_sources = [_][]const u8{"StandardLibrary.cpp"};

pub const refactoring_root = root ++ "Refactoring";
pub const refactoring_sources = [_][]const u8{
    "ASTSelection.cpp",
    "ASTSelectionRequirements.cpp",
    "AtomicChange.cpp",
    "Extract/Extract.cpp",
    "Extract/SourceExtraction.cpp",
    "Lookup.cpp",
    "RefactoringActions.cpp",
    "Rename/RenamingAction.cpp",
    "Rename/SymbolOccurrences.cpp",
    "Rename/USRFinder.cpp",
    "Rename/USRFindingAction.cpp",
    "Rename/USRLocFinder.cpp",
};

pub const syntax_root = root ++ "Syntax";
pub const syntax_sources = [_][]const u8{
    "BuildTree.cpp",
    "ComputeReplacements.cpp",
    "Nodes.cpp",
    "Mutations.cpp",
    "TokenBufferTokenManager.cpp",
    "Synthesis.cpp",
    "Tokens.cpp",
    "Tree.cpp",
};

pub const transformer_root = root ++ "Transformer";
pub const transformer_sources = [_][]const u8{
    "Parsing.cpp",
    "RangeSelector.cpp",
    "RewriteRule.cpp",
    "SourceCode.cpp",
    "SourceCodeBuilders.cpp",
    "Stencil.cpp",
    "Transformer.cpp",
};

const nodes_td = "clang/include/clang/Tooling/Syntax/Nodes.td";

pub const synthesize_node_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "ClangSyntaxNodeList",
            .td_file = nodes_td,
            .instruction = .{ .action = "-gen-clang-syntax-node-list" },
        },
        .virtual_path = "clang/Tooling/Syntax/Nodes.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangSyntaxNodeClasses",
            .td_file = nodes_td,
            .instruction = .{ .action = "-gen-clang-syntax-node-classes" },
        },
        .virtual_path = "clang/Tooling/Syntax/NodeClasses.inc",
    },
};
