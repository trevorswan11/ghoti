#pragma once

#include <filesystem>
#include <string>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>

#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/astgen.hh"

namespace ghoti::uir::sema {

// What UIR Sema analyzes: the root module and every module AstGen lowered for it
struct program {
    gsl::not_null<const mod::module_t*> root;
    gsl::span<const lowered_module>     modules;
};

struct options {
    // Every non-generic declaration is a root, not only what `main`, exports and tests reach
    bool check_all{true};
};

// A construct this phase of UIR Sema doesn't analyze yet, which stops the analysis
struct unsupported {
    std::string                   what;
    std::filesystem::path         path;
    stdx::option<source_location> where;
};

// One of: the program's GIR, the diagnostics that stopped it, or the first unsupported construct
struct outcome {
    stdx::option<gir::module_t> gir;
    ghoti::sema::diagnostics    diagnostics;
    stdx::option<unsupported>   stopped_at;

    [[nodiscard]] auto succeeded() const noexcept -> bool { return gir.has_value(); }
};

// Analyzes `prog` into one GIR module, creating types and constants through `ctx`
[[nodiscard]] auto analyze(ghoti::sema::context& ctx, const program& prog, const options& opts = {})
    -> outcome;

} // namespace ghoti::uir::sema
