#pragma once

#include <string>
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
    // Lowers what parsed of a module with syntax errors too, as the editor wants
    bool lenient{false};
};

// A name lowering resolved lexically, for the LSP. `use` names what `name` declares, or is a
// declared name itself when `name` is none
struct name_use {
    source                              use;
    stdx::option<ast::node_id>          name{};
    stdx::option<ast::node_id>          decl{};
    stdx::option<ast::explicit_type_id> annotation{};
};

// A module's UIR and every diagnostic lowering reported
struct lowered {
    module_t              uir;
    sema::diagnostics     diagnostics;
    std::vector<name_use> names{};
};

// Lowers `mod`'s AST to UIR, building its lexical scopes as it goes. Imports are loaded only
[[nodiscard]] auto lower_module(const mod::module_t& mod, const astgen_context& cx) -> lowered;

struct lowered_module {
    gsl::not_null<mod::module_t*> source;
    lowered                       result;
};

// Records what lowering resolved into the module's LSP tables
auto index_names(lowered_module& item) -> void;

// Lowers `root` and every module it imports, directly or not, once each and in load order
[[nodiscard]] auto lower_reachable(mod::module_t& root, const astgen_context& cx)
    -> std::vector<lowered_module>;

// Tried to load the program's entry code `std/start.gh` and adds what it reaches to `lowered`
[[nodiscard]] auto lower_start(const astgen_context& cx, std::vector<lowered_module>& lowered)
    -> stdx::result<void, std::string>;

// `lower_module`, failing with the diagnostics when there are any
[[nodiscard]] auto lower(const mod::module_t& mod, const astgen_context& cx)
    -> stdx::result<module_t, sema::diagnostics>;

} // namespace ghoti::uir
