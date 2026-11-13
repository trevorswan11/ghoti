#pragma once

#include <vector>

#include <gsl/pointers>
#include <stdx/result.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir {

// What lowering one module needs besides its AST: `@cfg`'s view of the build, and the manager
// that loads what the module imports
struct astgen_context {
    const sema::cfg_context& cfg;
    mod::module_manager&     modules;
};

// A module's UIR and every diagnostic lowering reported. The UIR is complete even when there are
// errors: an unresolved name lowers to a placeholder, so later passes and the LSP can still use it
struct lowered {
    module_t            uir;
    sema::diagnostics diagnostics;
};

// Lowers `mod`'s AST to UIR, building its lexical scopes as it goes. Imports are loaded but not
// lowered; each module is lowered on its own
[[nodiscard]] auto lower_module(const mod::module_t& mod, const astgen_context& cx) -> lowered;

// One module of a program, lowered
struct lowered_module {
    gsl::not_null<const mod::module_t*> source;
    lowered                             result;
};

// Lowers `root` and every module it imports, directly or not, once each and in load order. A
// module whose source failed to parse is skipped; its syntax errors are already its own
[[nodiscard]] auto lower_reachable(const mod::module_t& root, const astgen_context& cx)
    -> std::vector<lowered_module>;

// `lower_module`, failing with the diagnostics when there are any
[[nodiscard]] auto lower(const mod::module_t& mod, const astgen_context& cx)
    -> stdx::result<module_t, sema::diagnostics>;

} // namespace ghoti::uir
