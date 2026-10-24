#include "compiler/sema/pack_slice.hh"

#include <string_view>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token_type.hh"
#include "support/int128.hh"

namespace ghoti::sema {

auto pack_expansion_operand_of(const ast::AST& tree, ast::node_id expr)
    -> stdx::option<pack_expansion_operand> {
    if (const auto ident{tree.get_as_opt<ast::identifier_expr>(expr)}) {
        return pack_expansion_operand{.pack = ident->name, .range = stdx::none};
    }
    const auto index{tree.get_as_opt<ast::index_expr>(expr)};
    if (!index || !tree.get_as_opt<ast::range_expr>(index->index)) { return stdx::none; }
    const auto ident{tree.get_as_opt<ast::identifier_expr>(index->array)};
    if (!ident) { return stdx::none; }
    return pack_expansion_operand{.pack = ident->name, .range = index->index};
}

auto fold_pack_slice(gir::const_eval& evaluator,
                     const ast::AST&  tree,
                     ast::expr_handle range,
                     std::string_view pack,
                     usize            len) -> stdx::result<pack_slice, diagnostic> {
    const auto& bounds{tree.get_as<ast::range_expr>(range)};
    const auto  fold_bound{
        [&](stdx::option<ast::expr_handle> bound, i128 fallback) -> stdx::result<i128, diagnostic> {
            if (!bound) { return fallback; }
            const auto folded{evaluator.try_eval(*bound)};
            const auto value{folded ? folded->as_int_opt() : stdx::none};
            if (!value) {
                return make_sema_err(
                    fmt::format("'{}[lo..hi]' requires compile-time constant bounds", pack),
                    error::PACK_INDEX_NOT_CONST,
                    tree.location_of(*bound));
            }
            return *value;
        }};

    const bool inclusive{(*range).get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
    const auto lo{TRY(fold_bound(bounds.lhs, 0))};
    const auto hi{TRY(fold_bound(bounds.rhs, static_cast<i128>(len))) +
                  (inclusive && bounds.rhs ? 1 : 0)};
    if (lo < 0 || hi > static_cast<i128>(len) || lo > hi) {
        return make_sema_err(fmt::format("pack slice {}..{} out of range for '{}' (len {})",
                                         static_cast<i64>(lo),
                                         static_cast<i64>(hi),
                                         pack,
                                         len),
                             error::PACK_INDEX_OUT_OF_RANGE,
                             tree.location_of(range));
    }
    return pack_slice{.lo = static_cast<usize>(lo), .hi = static_cast<usize>(hi)};
}

} // namespace ghoti::sema
