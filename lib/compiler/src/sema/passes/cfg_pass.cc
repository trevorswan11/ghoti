#include "compiler/sema/passes/cfg_pass.hh"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

namespace {

using syntax::token_type_t;

// A human-readable name for a `cfgval::value`'s alternative, for diagnostics.
[[nodiscard]] auto value_type_name(const cfgval::value& value) -> std::string_view {
    return value.visit([](bool) -> std::string_view { return "bool"; },
                       [](i64) -> std::string_view { return "comptime int"; },
                       [](cfgval::member) -> std::string_view { return "enum member"; },
                       [](cfgval::text) -> std::string_view { return "[:0]u8"; },
                       [](cfgval::diverges) -> std::string_view { return "noreturn"; });
}

[[nodiscard]] auto is_cfg_atom(std::string_view name) -> bool {
    return name == "os" || name == "arch" || name == "abi" || name == "family" ||
           name == "endian" || name == "ptr_bits";
}

struct atom_enum_info {
    std::string_view                  type_name;
    std::span<const std::string_view> members;
};

constexpr std::array<std::string_view, 14> OS_MEMBERS{"linux",
                                                      "macos",
                                                      "ios",
                                                      "windows",
                                                      "freebsd",
                                                      "openbsd",
                                                      "netbsd",
                                                      "dragonfly",
                                                      "solaris",
                                                      "haiku",
                                                      "wasi",
                                                      "emscripten",
                                                      "uefi",
                                                      "freestanding"};
constexpr std::array<std::string_view, 16> ARCH_MEMBERS{"x86_64",
                                                        "x86",
                                                        "aarch64",
                                                        "arm",
                                                        "thumb",
                                                        "riscv64",
                                                        "riscv32",
                                                        "wasm32",
                                                        "wasm64",
                                                        "powerpc64",
                                                        "powerpc",
                                                        "mips64",
                                                        "mips",
                                                        "s390x",
                                                        "loongarch64",
                                                        "sparc64"};
constexpr std::array<std::string_view, 15> ABI_MEMBERS{"gnu",
                                                       "gnueabi",
                                                       "gnueabihf",
                                                       "gnux32",
                                                       "gnuabi64",
                                                       "musl",
                                                       "musleabi",
                                                       "musleabihf",
                                                       "msvc",
                                                       "android",
                                                       "androideabi",
                                                       "eabi",
                                                       "eabihf",
                                                       "itanium",
                                                       "none"};
constexpr std::array<std::string_view, 4>  FAMILY_MEMBERS{"unix", "windows", "wasm", "other"};
constexpr std::array<std::string_view, 2>  ENDIAN_MEMBERS{"little", "big"};

[[nodiscard]] auto atom_enum(std::string_view atom) -> stdx::option<atom_enum_info> {
    if (atom == "os") { return atom_enum_info{"Os", OS_MEMBERS}; }
    if (atom == "arch") { return atom_enum_info{"Arch", ARCH_MEMBERS}; }
    if (atom == "abi") { return atom_enum_info{"Abi", ABI_MEMBERS}; }
    if (atom == "family") { return atom_enum_info{"Family", FAMILY_MEMBERS}; }
    if (atom == "endian") { return atom_enum_info{"Endian", ENDIAN_MEMBERS}; }
    return stdx::none; // `ptr_bits` is integer-typed, not an enum
}

// The closest canonical member to `needle`, if one is within a small edit distance.
[[nodiscard]] auto closest_member(const atom_enum_info& info, std::string_view needle)
    -> stdx::option<std::string_view> {
    stdx::option<std::string_view> best;
    usize                          best_dist{3}; // suggest only for distance <= 2
    for (const auto candidate : info.members) {
        const auto dist{string_utils::edit_distance(needle, candidate)};
        if (dist < best_dist) {
            best_dist = dist;
            best.emplace(candidate);
        }
    }
    return best;
}

// The complementary operator for a swapped ordered comparison
[[nodiscard]] auto flip_ordered(token_type_t op) -> token_type_t {
    switch (op) {
    case token_type_t::LT:    return token_type_t::GT;
    case token_type_t::GT:    return token_type_t::LT;
    case token_type_t::LT_EQ: return token_type_t::GT_EQ;
    case token_type_t::GT_EQ: return token_type_t::LT_EQ;
    default:                  return op; // EQ / NEQ are symmetric
    }
}

} // namespace

auto cfg_pass::run(mod::module& module, context& ctx) -> bool {
    PROFILE_FUNCTION();
    cfg_pass pass{module, ctx};
    pass.gather_cfg_values(module.ast.roots_mut());
    for (const auto& [name, node] : pass.cfg_value_decls_) { pass.resolve_cfg_value(node); }
    pass.rewrite_roots(module.ast.roots_mut());
    return pass.ok_;
}

auto cfg_pass::gather_cfg_values(const std::vector<ast::node_id>& list) -> void {
    PROFILE_FUNCTION();
    for (const auto id : list) {
        const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(id)};
        if (!decl || !decl->value) { continue; }
        if (module_.ast.get_as_opt<ast::cfg_value_expr>(*decl->value)) {
            const auto& name{module_.ast.get_as<ast::identifier_expr>(decl->name).name};
            cfg_value_decls_.try_emplace(name, *decl->value);
        }
    }
}

auto cfg_pass::resolve_cfg_value(ast::node_id node) -> stdx::option<cfg_value> {
    PROFILE_FUNCTION();
    const auto key{node.get_index()};
    if (const auto done{cfg_value_cache_.find(key)}; done != cfg_value_cache_.end()) {
        return done->second;
    }
    if (!in_progress_.emplace(key).second) {
        fail(node, error::CFG_VALUE_CYCLE, "@cfgValue constant depends on itself");
        return stdx::none;
    }

    const auto&             expr{module_.ast.get_as<ast::cfg_value_expr>(node)};
    stdx::option<cfg_value> result;
    mod::cfg_value_result   record;

    if (expr.predicate) {
        if (const auto b{eval_predicate(*expr.predicate)}) {
            record.is_predicate = true;
            record.boolean      = *b;
            result              = cfg_value{*b};
        }
    } else {
        record.is_predicate = false;

        const bool arms_ok{check_guard_arm_types(node, expr)};
        if (!expr.fallback) {
            fail(node,
                 error::CFG_VALUE_MISSING_FALLBACK,
                 "@cfgValue guard form requires an '_ =>' fallback arm");
        }

        if (arms_ok && expr.fallback) {
            bool matched{false};
            for (const auto& guard : expr.guards) {
                const auto b{eval_predicate(guard.predicate)};
                if (!b) {
                    matched = true; // error already reported; stop scanning
                    break;
                }
                if (*b) {
                    record.chosen = guard.value;
                    result        = eval_term(guard.value);
                    matched       = true;
                    break;
                }
            }
            if (!matched) {
                record.chosen = *expr.fallback;
                result        = eval_term(*expr.fallback);
            }
        }
    }

    in_progress_.erase(key);
    if (result) {
        cfg_value_cache_.emplace(key, *result);
        module_.cfg_value_results.emplace(key, record);
    }
    return result;
}

auto cfg_pass::is_compile_error_call(ast::expr_handle h) -> bool {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    if (id.get_kind() != ast::node_kind::CALL_EXPRESSION) { return false; }
    const auto& call{module_.ast.get_as<ast::call_expr>(id)};
    return ast::node_id{call.function}.get_token_type() == token_type_t::BUILTIN_COMPILE_ERROR;
}

// Every guard arm (and the `_ =>` fallback) must yield the same `constexpr` type
auto cfg_pass::check_guard_arm_types(ast::node_id node, const ast::cfg_value_expr& expr) -> bool {
    PROFILE_FUNCTION();
    stdx::option<cfg_value> common;
    bool                    ok{true};

    const auto visit_arm{[&](ast::expr_handle value) -> void {
        if (is_compile_error_call(value)) { return; }
        const auto evaluated{eval_term(value)};
        if (!evaluated) {
            ok = false;
            return;
        }
        if (evaluated->is<cfgval::diverges>()) { return; }
        if (!common) {
            common.emplace(*evaluated);
            return;
        }
        if (common->index() != evaluated->index()) {
            fail(node,
                 error::CFG_VALUE_GUARD_TYPE_MISMATCH,
                 "@cfgValue guard arms must all yield the same type; found {} and {}",
                 value_type_name(*common),
                 value_type_name(*evaluated));
            ok = false;
        }
    }};

    for (const auto& guard : expr.guards) { visit_arm(guard.value); }
    if (expr.fallback) { visit_arm(*expr.fallback); }
    return ok;
}

auto cfg_pass::atom_value_str(std::string_view atom) const -> std::string_view {
    if (atom == "os") { return facts_.os; }
    if (atom == "arch") { return facts_.arch; }
    if (atom == "abi") { return facts_.abi; }
    if (atom == "family") { return facts_.family; }
    return facts_.endian; // endian
}

auto cfg_pass::atom_value(std::string_view atom) -> cfg_value {
    PROFILE_FUNCTION();
    if (atom == "ptr_bits") { return cfg_value{static_cast<i64>(facts_.ptr_bits)}; }
    return cfg_value{cfgval::member{atom_value_str(atom)}};
}

auto cfg_pass::int_literal(ast::expr_handle h) -> stdx::option<i64> {
    PROFILE_FUNCTION();
    return module_.ast[h].visit(
        [](const auto&) -> stdx::option<i64> { return stdx::none; },
        [](const ast::i32_expr& e) -> stdx::option<i64> { return e.value; },
        [](const ast::i64_expr& e) -> stdx::option<i64> { return e.value; },
        [](const ast::isize_expr& e) -> stdx::option<i64> { return e.value; },
        [](const ast::u32_expr& e) -> stdx::option<i64> { return static_cast<i64>(e.value); },
        [](const ast::u64_expr& e) -> stdx::option<i64> { return static_cast<i64>(e.value); },
        [](const ast::usize_expr& e) -> stdx::option<i64> { return static_cast<i64>(e.value); },
        [](const ast::u8_expr& e) -> stdx::option<i64> { return e.value; });
}

auto cfg_pass::eval_term(ast::expr_handle h) -> stdx::option<cfg_value> {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    switch (id.get_kind()) {
    case ast::node_kind::IDENTIFIER_EXPRESSION: {
        const auto& name{module_.ast.get_as<ast::identifier_expr>(id).name};
        if (is_cfg_atom(name)) { return atom_value(name); }
        if (const auto ref{cfg_value_decls_.find(name)}; ref != cfg_value_decls_.end()) {
            return resolve_cfg_value(ref->second);
        }
        fail(id,
             error::CFG_UNKNOWN_ATOM,
             "unknown cfg atom '{}'; valid: os, arch, abi, family, endian, ptr_bits "
             "(or a @cfgValue constant)",
             name);
        return stdx::none;
    }
    case ast::node_kind::IMPLICIT_ACCESS_EXPRESSION: {
        const auto& access{module_.ast.get_as<ast::implicit_access_expr>(id)};
        return cfg_value{
            cfgval::member{module_.ast.get_as<ast::identifier_expr>(access.member).name}};
    }
    case ast::node_kind::BOOL_EXPRESSION:
        return cfg_value{id.get_token_type() == token_type_t::BOOLEAN_TRUE};
    case ast::node_kind::STRING_EXPRESSION:
        return cfg_value{cfgval::text{module_.ast.get_as<ast::string_expr>(id).value}};
    case ast::node_kind::UNARY_EXPRESSION:
    case ast::node_kind::BINARY_EXPRESSION:
        if (const auto b{eval_predicate(h)}) { return cfg_value{*b}; }
        return stdx::none;
    case ast::node_kind::CALL_EXPRESSION: {
        const auto& call{module_.ast.get_as<ast::call_expr>(id)};
        if (ast::node_id{call.function}.get_token_type() == token_type_t::BUILTIN_COMPILE_ERROR) {
            fire_compile_error(id, call);
            return cfg_value{cfgval::diverges{}};
        }
        break;
    }
    default: break;
    }

    if (const auto i{int_literal(h)}) { return cfg_value{*i}; }
    fail(id,
         error::CFG_ILLEGAL_PREDICATE,
         "cfg predicates may only use target atoms, literals, operators, and @cfgValue "
         "constants");
    return stdx::none;
}

auto cfg_pass::eval_predicate(ast::expr_handle h) -> stdx::option<bool> {
    PROFILE_FUNCTION();
    const ast::node_id id{h};

    if (id.get_kind() == ast::node_kind::UNARY_EXPRESSION) {
        const auto& unary{module_.ast.get_as<ast::unary_expr>(id)};
        if (id.get_token_type() != token_type_t::BANG) {
            fail(id, error::CFG_ILLEGAL_PREDICATE, "unsupported operator in cfg predicate");
            return stdx::none;
        }
        const auto inner{eval_predicate(unary.rhs)};
        return inner ? stdx::option<bool>{!*inner} : stdx::none;
    }

    if (id.get_kind() == ast::node_kind::BINARY_EXPRESSION) {
        const auto& bin{module_.ast.get_as<ast::binary_expr>(id)};
        const auto  op{id.get_token_type()};
        if (op == token_type_t::BOOLEAN_AND || op == token_type_t::BOOLEAN_OR) {
            const auto lhs{eval_predicate(bin.lhs)};
            const auto rhs{eval_predicate(bin.rhs)};
            if (!lhs || !rhs) { return stdx::none; }
            return op == token_type_t::BOOLEAN_AND ? (*lhs && *rhs) : (*lhs || *rhs);
        }
        return eval_comparison(id, op, bin.lhs, bin.rhs);
    }

    const auto term{eval_term(h)};
    if (!term) { return stdx::none; }
    if (const auto boolean{term->as_opt<bool>()}) { return *boolean; }
    fail(id, error::CFG_ILLEGAL_PREDICATE, "cfg predicate does not evaluate to a boolean");
    return stdx::none;
}

auto cfg_pass::classify_operand(ast::expr_handle h) -> operand {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    switch (id.get_kind()) {
    case ast::node_kind::IDENTIFIER_EXPRESSION: {
        const auto& name{module_.ast.get_as<ast::identifier_expr>(id).name};
        if (name == "ptr_bits") { return {.tag = operand::kind::PTR_BITS}; }
        if (is_cfg_atom(name)) { return {.tag = operand::kind::ATOM, .text = name}; }
        if (const auto ref{cfg_value_decls_.find(name)}; ref != cfg_value_decls_.end()) {
            const auto value{resolve_cfg_value(ref->second)};
            if (!value) { return {}; } // error already reported
            return value->visit(
                [](bool b) -> operand { return {.tag = operand::kind::BOOL, .boolean = b}; },
                [](i64 i) -> operand { return {.tag = operand::kind::INT, .integer = i}; },
                [](cfgval::member m) -> operand {
                    return {.tag = operand::kind::MEMBER, .text = m.name};
                },
                [](cfgval::text) -> operand { return {.tag = operand::kind::STRING}; },
                [](cfgval::diverges) -> operand { return {}; });
        }
        fail(id,
             error::CFG_UNKNOWN_ATOM,
             "unknown cfg atom '{}'; valid: os, arch, abi, family, endian, ptr_bits "
             "(or a @cfgValue constant)",
             name);
        return {};
    }
    case ast::node_kind::IMPLICIT_ACCESS_EXPRESSION: {
        const auto& access{module_.ast.get_as<ast::implicit_access_expr>(id)};
        return {.tag  = operand::kind::MEMBER,
                .text = module_.ast.get_as<ast::identifier_expr>(access.member).name};
    }
    case ast::node_kind::BOOL_EXPRESSION:
        return {.tag     = operand::kind::BOOL,
                .boolean = id.get_token_type() == token_type_t::BOOLEAN_TRUE};
    case ast::node_kind::STRING_EXPRESSION: return {.tag = operand::kind::STRING};
    default:                                break;
    }
    if (const auto i{int_literal(h)}) { return {.tag = operand::kind::INT, .integer = *i}; }
    return {}; // ERROR, caller decides whether to report
}

auto cfg_pass::eval_comparison(ast::node_id     at,
                               token_type_t     op,
                               ast::expr_handle lhs_h,
                               ast::expr_handle rhs_h) -> stdx::option<bool> {
    PROFILE_FUNCTION();
    const bool was_ok{ok_};
    auto       lhs{classify_operand(lhs_h)};
    auto       rhs{classify_operand(rhs_h)};
    using ok = operand::kind;

    if (lhs.tag == ok::ERROR || rhs.tag == ok::ERROR) {
        if (was_ok && ok_) {
            fail(at,
                 error::CFG_ILLEGAL_PREDICATE,
                 "cfg comparison operands must be target atoms, enum members, or integer "
                 "literals");
        }
        return stdx::none;
    }

    if (lhs.tag == ok::STRING || rhs.tag == ok::STRING) {
        fail(at,
             error::CFG_COMPARISON_TYPE_MISMATCH,
             "cfg comparisons use enum members like `.linux`, not strings");
        return stdx::none;
    }
    const bool eq_op{op == token_type_t::EQ || op == token_type_t::NEQ};

    // Put the atom / ptr_bits side on the left, flipping ordered operators to match.
    auto effective_op{op};
    if ((lhs.tag == ok::MEMBER || lhs.tag == ok::INT || lhs.tag == ok::BOOL) &&
        (rhs.tag == ok::ATOM || rhs.tag == ok::PTR_BITS)) {
        std::swap(lhs, rhs);
        effective_op = flip_ordered(op);
    }

    if (lhs.tag == ok::ATOM) {
        const auto info{*atom_enum(lhs.text)}; // atoms always have enum info
        if (!eq_op) {
            fail(at,
                 error::CFG_COMPARISON_TYPE_MISMATCH,
                 "'{}' has type {} and supports only '==' / '!=', not ordered comparisons",
                 lhs.text,
                 info.type_name);
            return stdx::none;
        }
        if (rhs.tag == ok::INT) {
            fail(at,
                 error::CFG_COMPARISON_TYPE_MISMATCH,
                 "'{}' has type {} and cannot be compared with an integer",
                 lhs.text,
                 info.type_name);
            return stdx::none;
        }
        if (rhs.tag != ok::MEMBER) {
            fail(at,
                 error::CFG_COMPARISON_TYPE_MISMATCH,
                 "'{}' has type {} and can only be compared with one of its members "
                 "(e.g. `{} == .{}`)",
                 lhs.text,
                 info.type_name,
                 lhs.text,
                 info.members.front());
            return stdx::none;
        }
        if (std::ranges::find(info.members, rhs.text) == info.members.end()) {
            if (const auto suggestion{closest_member(info, rhs.text)}) {
                fail(at,
                     error::CFG_UNKNOWN_ENUM_MEMBER,
                     "'.{}' is not a member of {}; did you mean '.{}'?",
                     rhs.text,
                     info.type_name,
                     *suggestion);
            } else {
                fail(at,
                     error::CFG_UNKNOWN_ENUM_MEMBER,
                     "'.{}' is not a member of {}",
                     rhs.text,
                     info.type_name);
            }
            return stdx::none;
        }
        const bool equal{atom_value_str(lhs.text) == rhs.text};
        return op == token_type_t::EQ ? equal : !equal;
    }

    if (lhs.tag == ok::PTR_BITS) {
        if (rhs.tag != ok::INT) {
            fail(at,
                 error::CFG_COMPARISON_TYPE_MISMATCH,
                 "'ptr_bits' is an integer and can only be compared with an integer literal");
            return stdx::none;
        }
        const i64 l{static_cast<i64>(facts_.ptr_bits)};
        const i64 r{rhs.integer};
        switch (effective_op) {
        case token_type_t::EQ:    return l == r;
        case token_type_t::NEQ:   return l != r;
        case token_type_t::LT:    return l < r;
        case token_type_t::GT:    return l > r;
        case token_type_t::LT_EQ: return l <= r;
        case token_type_t::GT_EQ: return l >= r;
        default:                  return stdx::none;
        }
    }

    if (lhs.tag == ok::BOOL && rhs.tag == ok::BOOL) {
        if (!eq_op) {
            fail(at,
                 error::CFG_COMPARISON_TYPE_MISMATCH,
                 "booleans support only '==' / '!=', not ordered comparisons");
            return stdx::none;
        }
        return op == token_type_t::EQ ? lhs.boolean == rhs.boolean : lhs.boolean != rhs.boolean;
    }

    fail(at,
         error::CFG_COMPARISON_TYPE_MISMATCH,
         "cfg comparison needs a target atom on one side (e.g. `os == .linux`, "
         "`ptr_bits >= 32`)");
    return stdx::none;
}

auto cfg_pass::compile_error_message(const ast::call_expr& call) -> stdx::option<std::string_view> {
    PROFILE_FUNCTION();
    if (call.arguments.empty()) { return stdx::none; }
    const auto arg{call.arguments.front().as_opt<ast::expr_handle>()};
    if (!arg) { return stdx::none; }
    if (const auto str{module_.ast.get_as_opt<ast::string_expr>(*arg)}) { return str->value; }
    if (const auto ident{module_.ast.get_as_opt<ast::identifier_expr>(*arg)}) {
        if (const auto ref{cfg_value_decls_.find(ident->name)}; ref != cfg_value_decls_.end()) {
            if (const auto value{resolve_cfg_value(ref->second)}) {
                if (const auto str{value->as_opt<cfgval::text>()}) { return str->value; }
            }
        }
    }
    return stdx::none;
}

auto cfg_pass::fire_compile_error(ast::node_id at, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    ok_ = false;
    if (const auto message{compile_error_message(call)}) {
        return ctx_.diags.emplace_back(
            std::string{*message}, error::COMPILE_ERROR_REACHED, module_.ast.location_of(at));
    }

    ctx_.diags.emplace_back(
        "@compileError requires a compile-time-known string message (a string literal or a "
        "@cfgValue string constant)",
        error::COMPILE_ERROR_NON_CONSTANT_MESSAGE,
        module_.ast.location_of(at));
}

auto cfg_pass::fire_eager_compile_errors(const std::vector<ast::stmt_handle>& items) -> void {
    PROFILE_FUNCTION();
    for (const auto item : items) {
        const auto expr_stmt{module_.ast.get_as_opt<ast::expr_stmt>(item)};
        if (!expr_stmt) { continue; }
        const auto call{module_.ast.get_as_opt<ast::call_expr>(expr_stmt->expression)};
        if (call &&
            ast::node_id{call->function}.get_token_type() == token_type_t::BUILTIN_COMPILE_ERROR) {
            fire_compile_error(ast::node_id{expr_stmt->expression}, *call);
        }
    }
}

auto cfg_pass::rewrite_items(std::vector<ast::stmt_handle>& list) -> void {
    PROFILE_FUNCTION();
    std::vector<ast::stmt_handle> out;
    out.reserve(list.size());
    for (const auto stmt : list) {
        if (const auto cfg{module_.ast.get_as_opt<ast::cfg_stmt>(stmt)}) {
            if (const auto arm{select_arm_of(cfg->arms)}) {
                auto items{arm->items};
                rewrite_items(items);
                fire_eager_compile_errors(items);
                for (const auto spliced : items) { out.emplace_back(spliced); }
            }
            continue; // the @cfg node itself is dropped
        }
        recurse_into_bodies(stmt);
        out.emplace_back(stmt);
    }
    list = std::move(out);
}

auto cfg_pass::rewrite_roots(std::vector<ast::node_id>& roots) -> void {
    PROFILE_FUNCTION();
    std::vector<ast::stmt_handle> as_items;
    as_items.reserve(roots.size());
    for (const auto id : roots) { as_items.emplace_back(ast::stmt_handle{id}); }
    rewrite_items(as_items);
    roots.assign(as_items.begin(), as_items.end());
}

auto cfg_pass::recurse_into_bodies(ast::stmt_handle stmt) -> void {
    PROFILE_FUNCTION();
    const ast::node_id id{stmt};
    if (id.get_kind() == ast::node_kind::BLOCK_STATEMENT) {
        rewrite_items(module_.ast.get_as_mut<ast::block_stmt>(id).statements);
        return;
    }
    if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(id)}; decl && decl->value) {
        recurse_into_expr(*decl->value);
    }
    if (const auto expr_st{module_.ast.get_as_opt<ast::expr_stmt>(id)}) {
        recurse_into_expr(expr_st->expression);
    }
    if (const auto test{module_.ast.get_as_opt<ast::test_stmt>(id)}) {
        rewrite_items(
            module_.ast.get_as_mut<ast::block_stmt>(ast::node_id{test->block}).statements);
    }
}

auto cfg_pass::recurse_into_block(ast::block_handle block) -> void {
    PROFILE_FUNCTION();
    rewrite_items(module_.ast.get_as_mut<ast::block_stmt>(ast::node_id{block}).statements);
}

auto cfg_pass::recurse_into_members(const ast::member_list& members) -> void {
    PROFILE_FUNCTION();
    for (const auto member : members) {
        const ast::node_id mid{member};
        if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(mid)}; decl && decl->value) {
            recurse_into_expr(*decl->value);
        }
    }
}

auto cfg_pass::recurse_into_expr(ast::expr_handle expr) -> void {
    PROFILE_FUNCTION();
    module_.ast[expr].visit(
        [&](const ast::function_expr& fn) {
            if (!fn.is_type_expr) { recurse_into_block(fn.body); }
        },
        [&](const ast::if_expr& branch) {
            recurse_into_bodies(branch.consequence);
            if (branch.alternate) { recurse_into_bodies(*branch.alternate); }
        },
        [&](const ast::match_expr& match) {
            for (const auto& arm : match.arms) { recurse_into_bodies(arm.dispatch); }
        },
        [&](const ast::while_loop_expr& loop) {
            recurse_into_block(loop.block);
            if (loop.non_break) { recurse_into_bodies(*loop.non_break); }
        },
        [&](const ast::do_while_loop_expr& loop) { recurse_into_block(loop.block); },
        [&](const ast::for_loop_expr& loop) {
            recurse_into_block(loop.block);
            if (loop.non_break) { recurse_into_bodies(*loop.non_break); }
        },
        [&](const ast::infinite_loop_expr& loop) { recurse_into_block(loop.block); },
        [&](const ast::label_expr& label) {
            const ast::node_id body{label.body};
            if (body.get_kind() == ast::node_kind::BLOCK_STATEMENT) {
                recurse_into_block(ast::block_handle{body});
            } else {
                recurse_into_expr(ast::expr_handle{body});
            }
        },
        [&](ast::struct_expr& node) {
            flatten_cfg_groups(node.cfg_groups, node.fields);
            flatten_cfg_groups(node.member_cfg_groups, node.members);
            recurse_into_members(node.members);
        },
        [&](ast::union_expr& node) {
            flatten_cfg_groups(node.cfg_groups, node.fields);
            flatten_cfg_groups(node.member_cfg_groups, node.members);
            recurse_into_members(node.members);
        },
        [&](ast::enum_expr& node) {
            flatten_cfg_groups(node.cfg_groups, node.enumerations);
            flatten_cfg_groups(node.member_cfg_groups, node.members);
            recurse_into_members(node.members);
        },
        [](const auto&) {});
}

} // namespace ghoti::sema
