//! https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/clang/lib/Analysis
pub const root = "clang/lib/Analysis";
pub const sources = [_][]const u8{
    "AnalysisDeclContext.cpp",
    "AnnexKDetection.cpp",
    "BodyFarm.cpp",
    "CalledOnceCheck.cpp",
    "CFG.cpp",
    "CFGBackEdges.cpp",
    "CFGReachabilityAnalysis.cpp",
    "CFGStmtMap.cpp",
    "CallGraph.cpp",
    "CloneDetection.cpp",
    "CocoaConventions.cpp",
    "ConstructionContext.cpp",
    "Consumed.cpp",
    "CodeInjector.cpp",
    "Dominators.cpp",
    "ExprMutationAnalyzer.cpp",
    "FixitUtil.cpp",
    "IntervalPartition.cpp",
    "IssueHash.cpp",
    "LiveVariables.cpp",
    "MacroExpansionContext.cpp",
    "ObjCNoReturn.cpp",
    "PathDiagnostic.cpp",
    "PostOrderCFGView.cpp",
    "ProgramPoint.cpp",
    "ReachableCode.cpp",
    "RetainSummaryManager.cpp",
    "ThreadSafety.cpp",
    "ThreadSafetyCommon.cpp",
    "ThreadSafetyLogical.cpp",
    "ThreadSafetyTIL.cpp",
    "UninitializedValues.cpp",
    "UnsafeBufferUsage.cpp",
};

// Skipping FlowAnalysis (not necessary) and plugins (disabled)

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/Analysis/LifetimeSafety/CMakeLists.txt
pub const lifetime_safety_root = "clang/lib/Analysis/LifetimeSafety";
pub const lifetime_safety_sources = [_][]const u8{
    "Checker.cpp",
    "Facts.cpp",
    "FactsGenerator.cpp",
    "LifetimeAnnotations.cpp",
    "LifetimeSafety.cpp",
    "LiveOrigins.cpp",
    "LifetimeStats.cpp",
    "Loans.cpp",
    "LoanPropagation.cpp",
    "MovedLoans.cpp",
    "Origins.cpp",
};
