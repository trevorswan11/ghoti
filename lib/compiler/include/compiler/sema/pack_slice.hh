#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/sema/error.hh"

namespace ghoti::ast { class AST; }        // namespace ghoti::ast
namespace ghoti::gir { class const_eval; } // namespace ghoti::gir

namespace ghoti::sema {

// What `...` expands: a whole pack `rest`, or the slice `rest[lo..hi]` of one
struct pack_expansion_operand {
    std::string_view               pack;
    stdx::option<ast::expr_handle> range;
};

// `expr` read as a `...` operand, if it spells a bare identifier or a range index of one
[[nodiscard]] auto pack_expansion_operand_of(const ast::AST& tree, ast::node_id expr)
    -> stdx::option<pack_expansion_operand>;

// The half-open element positions `[lo, hi)` a pack slice selects
struct pack_slice {
    usize lo;
    usize hi;
};

// Folds the bounds of `pack[range]` against the pack's `len` elements
[[nodiscard]] auto fold_pack_slice(gir::const_eval& evaluator,
                                   const ast::AST&  tree,
                                   ast::expr_handle range,
                                   std::string_view pack,
                                   usize            len) -> stdx::result<pack_slice, diagnostic>;

} // namespace ghoti::sema
