#pragma once

#include <vector>

#include <gsl/pointers>
#include <stdx/result.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir {

// What lowering one module needs besides its AST
struct astgen_context {
    const sema::cfg_context& cfg;     // `@cfg`'s view of the build
    mod::module_manager&     modules; // the manager that loads what the module imports
};

// A module's UIR and every diagnostic lowering reported
struct lowered {
    module_t          uir;
    sema::diagnostics diagnostics;
};

// Lowers `mod`'s AST to UIR, building its lexical scopes as it goes. Imports are loaded only
[[nodiscard]] auto lower_module(const mod::module_t& mod, const astgen_context& cx) -> lowered;

struct lowered_module {
    gsl::not_null<const mod::module_t*> source;
    lowered                             result;
};

// Lowers `root` and every module it imports, directly or not, once each and in load order
[[nodiscard]] auto lower_reachable(const mod::module_t& root, const astgen_context& cx)
    -> std::vector<lowered_module>;

// `lower_module`, failing with the diagnostics when there are any
[[nodiscard]] auto lower(const mod::module_t& mod, const astgen_context& cx)
    -> stdx::result<module_t, sema::diagnostics>;

} // namespace ghoti::uir
