#include "compiler/codegen/llvm_optimizer.hh"

#include <fmt/format.h>
#include <string>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PassTimingInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/raw_ostream.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <string_view>

#include "compiler/codegen/opt_level.hh"
#include "compiler/sema/error.hh"

namespace ghoti::codegen {

namespace {

[[nodiscard]] constexpr auto to_llvm_opt_level(opt_level level) noexcept
    -> llvm::OptimizationLevel {
    switch (level) {
    case opt_level::O0: return llvm::OptimizationLevel::O0;
    case opt_level::O1: return llvm::OptimizationLevel::O1;
    case opt_level::O2: return llvm::OptimizationLevel::O2;
    case opt_level::O3: return llvm::OptimizationLevel::O3;
    case opt_level::Os: return llvm::OptimizationLevel::Os;
    case opt_level::Oz: return llvm::OptimizationLevel::Oz;
    }
    return llvm::OptimizationLevel::O0;
}

[[nodiscard]] auto verify_mod(llvm::Module& module, std::string_view phase)
    -> stdx::result<void, sema::diagnostic> {
    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    if (llvm::verifyModule(module, &os)) {
        return sema::make_sema_err(
            fmt::format("{}-optimization module verification failed: {}", phase, err_str),
            sema::error::CODEGEN_VERIFICATION_FAILED);
    }
    return {};
}

} // namespace

auto llvm_optimizer::optimize(llvm::Module& module, const optimizer_options& options)
    -> stdx::result<void, sema::diagnostic> {
    PROFILE_FUNCTION();

    if (options.verify_each) { TRY(verify_mod(module, "Pre")); }

    // Set up analysis managers
    llvm::LoopAnalysisManager     lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager    cgam;
    llvm::ModuleAnalysisManager   mam;

    // Set up instrumentations and callbacks if requested
    stdx::option<llvm::PassInstrumentationCallbacks> pic;
    stdx::option<llvm::StandardInstrumentations>     si;
    stdx::option<llvm::TimePassesHandler>            time_passes;

    if (options.debug_logging || options.time_passes) {
        pic.emplace();
        llvm::PrintPassOptions print_pass_opts;
        print_pass_opts.Verbose = options.debug_logging;
        si.emplace(context_, options.debug_logging, options.verify_each, print_pass_opts);
        si->registerCallbacks(*pic, &mam);

        if (options.time_passes) {
            time_passes.emplace(true);
            time_passes->registerCallbacks(*pic);
        }
    }

    // Set up pass builder
    llvm::PipelineTuningOptions pto;
    llvm::PassBuilder           pb{options.target_machine, pto, stdx::none, pic ? &*pic : nullptr};

    // Register all the basic analyses with the managers
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    const auto              llvm_level{to_llvm_opt_level(options.level)};
    llvm::ModulePassManager mpm{options.level == opt_level::O0
                                    ? pb.buildO0DefaultPipeline(llvm_level)
                                    : pb.buildPerModuleDefaultPipeline(llvm_level)};

    {
        PROFILE_SCOPE("Run LLVM Optimizations");
        mpm.run(module, mam);
    }

    if (options.time_passes && time_passes) { time_passes->print(); }

    // Clear analysis caches
    mam.clear();
    cgam.clear();
    fam.clear();
    lam.clear();

    if (options.verify_each) { TRY(verify_mod(module, "Post")); }
    return {};
}

} // namespace ghoti::codegen
