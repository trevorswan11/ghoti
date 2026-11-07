#include "compiler/sema/cfg_evaluator.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/cfg_atoms.hh"
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
           name == "endian" || name == "ptr_bits" || name == "optimize" || name == "safety" ||
           name == "testing";
}

struct atom_enum_info {
    std::string_view                  type_name;
    gsl::span<const std::string_view> members;
};

[[nodiscard]] auto atom_enum(std::string_view atom) -> stdx::option<atom_enum_info> {
    if (atom == "os") { return atom_enum_info{"Os", cfg_atoms::OS_MEMBERS}; }
    if (atom == "arch") { return atom_enum_info{"Arch", cfg_atoms::ARCH_MEMBERS}; }
    if (atom == "abi") { return atom_enum_info{"Abi", cfg_atoms::ABI_MEMBERS}; }
    if (atom == "family") { return atom_enum_info{"Family", cfg_atoms::FAMILY_MEMBERS}; }
    if (atom == "endian") { return atom_enum_info{"Endian", cfg_atoms::ENDIAN_MEMBERS}; }
    if (atom == "optimize") { return atom_enum_info{"OptimizeMode", cfg_atoms::OPTIMIZE_MEMBERS}; }
    return stdx::none; // `ptr_bits` is integer-typed, `safety` and `testing` are bools
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

auto cfg_context::from(const context& ctx) -> cfg_context {
    return {.target_opts    = ctx.target_opts,
            .facts          = codegen::target_facts::resolve(ctx.target_opts.triple_str),
            .build_mode     = ctx.build_mode,
            .runtime_safety = ctx.runtime_safety,
            .testing        = ctx.testing,
            .modules        = ctx.modules};
}

cfg_evaluator::cfg_evaluator(const mod::module_t& module, const cfg_context& cx, diagnostics& diags)
    : module_{module}, cx_{cx}, diags_{diags} {}

cfg_evaluator::cfg_evaluator(const mod::module_t& module,
                             const cfg_context&   cx,
                             diagnostics&         diags,
                             shared_state&        shared)
    : module_{module}, cx_{cx}, diags_{diags}, shared_{&shared} {}

auto cfg_evaluator::resolve_all_cfg_values() -> void {
    for (const auto& [name, decl] : cfg_value_decls_) { resolve_cfg_value(decl.value); }
}

auto cfg_evaluator::chosen_expression(ast::node_id node) -> stdx::option<ast::expr_handle> {
    DISCARD(resolve_cfg_value(node));
    const auto chosen{chosen_.find(node.get_index())};
    if (chosen == chosen_.end() || !chosen->second) { return stdx::none; }
    return ast::expr_handle{*chosen->second};
}

auto cfg_evaluator::gather_cfg_values(const std::vector<ast::node_id>& list) -> void {
    PROFILE_FUNCTION();
    for (const auto id : list) {
        const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(id)};
        if (!decl || !decl->value) { continue; }
        if (module_.ast.get_as_opt<ast::cfg_value_expr>(*decl->value)) {
            const auto& name{module_.ast.get_as<ast::identifier_expr>(decl->name).name};
            cfg_value_decls_.try_emplace(name, cfg_decl{id, *decl->value});
        }
    }
}

auto cfg_evaluator::resolve_cfg_value(ast::node_id node) -> stdx::option<cfg_value> {
    PROFILE_FUNCTION();
    const auto key{node.get_index()};
    if (const auto done{cfg_value_cache_.find(key)}; done != cfg_value_cache_.end()) {
        return done->second;
    }
    if (!shared_->in_progress.emplace(&module_, key).second) {
        shared_->hit_cycle = true;
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
                for (const auto predicate : guard.predicates) {
                    const auto b{eval_predicate(predicate)};
                    if (!b) {
                        matched = true; // error already reported; stop scanning
                        break;
                    }
                    if (*b) {
                        record.chosen = ast::node_id{guard.value};
                        result        = eval_term(guard.value);
                        matched       = true;
                        break;
                    }
                }
                if (matched) { break; }
            }
            if (!matched) {
                record.chosen = ast::node_id{*expr.fallback};
                result        = eval_term(*expr.fallback);
            }
        }
    }

    shared_->in_progress.erase({&module_, key});
    if (result) {
        cfg_value_cache_.emplace(key, *result);
        chosen_.emplace(key, record.chosen);
        if (recorded_on_) { recorded_on_->cfg_value_results.emplace(key, record); }
    }
    return result;
}

auto cfg_evaluator::to_operand(const cfg_value& value) -> operand {
    return value.visit(
        [](bool b) -> operand { return {.tag = operand::kind::BOOL, .boolean = b}; },
        [](i64 i) -> operand { return {.tag = operand::kind::INT, .integer = i}; },
        [](cfgval::member m) -> operand { return {.tag = operand::kind::MEMBER, .text = m.name}; },
        [](cfgval::text) -> operand { return {.tag = operand::kind::STRING}; },
        [](cfgval::diverges) -> operand { return {}; });
}

// The module a module-scope import binds to `name`, looking through `@cfg` arms that are taken
auto cfg_evaluator::find_import(std::string_view name, bool public_only)
    -> stdx::option<mod::module_t&> {
    std::vector<ast::stmt_handle> roots;
    for (const auto id : module_.ast.get_roots()) { roots.emplace_back(ast::stmt_handle{id}); }
    return find_import_in(roots, name, public_only);
}

// A predicate can't use an import its own gate guards, so a statement still being selected is
// skipped
auto cfg_evaluator::taken_arm(ast::node_id id, const ast::cfg_stmt& cfg) -> stdx::opt_size {
    const std::pair key{gsl::not_null<const mod::module_t*>{&module_}, id.get_index()};
    if (const auto seen{shared_->arms.find(key)}; seen != shared_->arms.end()) {
        return seen->second.selected ? seen->second.arm : stdx::opt_size{};
    }
    shared_->arms.emplace(key, shared_state::arm_choice{});
    stdx::opt_size taken;
    if (const auto arm{select_arm_of(cfg.arms)}) {
        taken.emplace(static_cast<usize>(arm.get() - cfg.arms.data()));
    }
    shared_->arms[key] = {.selected = true, .arm = taken};
    return taken;
}

auto cfg_evaluator::find_import_in(const std::vector<ast::stmt_handle>& items,
                                   std::string_view                     name,
                                   bool public_only) -> stdx::option<mod::module_t&> {
    for (const auto item : items) {
        const ast::node_id id{item};
        if (const auto cfg{module_.ast.get_as_opt<ast::cfg_stmt>(id)}) {
            const auto arm{taken_arm(id, *cfg)};
            if (!arm) { continue; }
            if (auto found{find_import_in(cfg->arms[*arm].items, name, public_only)}) {
                return found;
            }
            continue;
        }
        const auto import_node{module_.ast.get_as_opt<ast::import_stmt>(id)};
        if (!import_node) { continue; }
        const auto named{import_node->get_name(module_.ast)};
        if (!named || named->second != name) { continue; }
        if (public_only && !ast::import_stmt::is_public(id)) { return stdx::none; }

        const auto loaded{[&] {
            if (const auto path{module_.ast.get_as_opt<ast::string_expr>(import_node->payload)}) {
                return cx_.modules.try_get_file_module(path->value, module_.parent_path);
            }
            return cx_.modules.try_get_library_module(
                module_.ast.get_as<ast::identifier_expr>(import_node->payload).name);
        }()};
        if (!loaded || (*loaded)->is_errored()) { return stdx::none; }
        return **loaded;
    }
    return stdx::none;
}

// `mod.NAME` / `mod.sub.NAME`: a `pub` `@cfgValue` constant of an imported module, through
// `pub import`s after the first step. Its own errors are reported when its module is collected.
auto cfg_evaluator::resolve_foreign_value(ast::node_id dot) -> stdx::option<cfg_value> {
    PROFILE_FUNCTION();
    std::vector<std::string_view> path;
    ast::node_id                  at{dot};
    while (const auto access{module_.ast.get_as_opt<ast::dot_expr>(at)}) {
        path.emplace_back(module_.ast.get_as<ast::identifier_expr>(access->member).name);
        at = ast::node_id{access->object};
    }
    const auto root{module_.ast.get_as_opt<ast::identifier_expr>(at)};
    if (!root) {
        fail(dot,
             error::CFG_ILLEGAL_CFG_VALUE_REFERENCE,
             "a cfg predicate can only reference another module's `@cfgValue` constant as "
             "`module.NAME`");
        return stdx::none;
    }
    path.emplace_back(root->name);
    std::ranges::reverse(path);
    const auto spelled{fmt::format("{}", fmt::join(path, "."))};

    // Each step reads the next module with a throwaway evaluator, whose errors that module reports
    diagnostics                         scratch_diags;
    gsl::not_null<const mod::module_t*> current{&module_};
    stdx::option<mod::module_t&>        foreign;
    for (usize i{0}; i + 1 < path.size(); ++i) {
        const bool    is_foreign{current != &module_};
        cfg_evaluator step{*current, cx_, scratch_diags, *shared_};
        const auto    next{step.find_import(path[i], is_foreign)};
        if (!next) {
            fail(dot,
                 error::CFG_ILLEGAL_CFG_VALUE_REFERENCE,
                 "'{}' in '{}' isn't a{} module import",
                 path[i],
                 spelled,
                 is_foreign ? " public" : "");
            return stdx::none;
        }
        current = next.get();
        foreign = next;
    }

    cfg_evaluator owner{*current, cx_, scratch_diags, *shared_};
    if (recorded_on_ && foreign) { owner.record_results_on(*foreign); }
    owner.gather_cfg_values(current->ast.get_roots());
    const auto decl{owner.cfg_value_decls_.find(path.back())};
    if (decl == owner.cfg_value_decls_.end() ||
        !current->ast.get_as<ast::decl_stmt>(decl->second.decl)
             .has_modifier(ast::decl_modifiers::PUBLIC)) {
        fail(dot,
             error::CFG_ILLEGAL_CFG_VALUE_REFERENCE,
             "'{}' isn't a public `@cfgValue` constant, which is all a cfg predicate can read "
             "from another module",
             spelled);
        return stdx::none;
    }

    const auto value{owner.resolve_cfg_value(decl->second.value)};
    if (!value) {
        if (shared_->hit_cycle) {
            fail(dot, error::CFG_VALUE_CYCLE, "'{}' depends on itself", spelled);
        } else {
            fail(dot, error::CFG_ILLEGAL_CFG_VALUE_REFERENCE, "'{}' has errors", spelled);
        }
    }
    return value;
}

auto cfg_evaluator::is_compile_error_call(ast::expr_handle h) -> bool {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    if (id.get_kind() != ast::node_kind::CALL_EXPRESSION) { return false; }
    const auto& call{module_.ast.get_as<ast::call_expr>(id)};
    return ast::node_id{call.function}.get_builtin() == syntax::builtin_t::COMPILE_ERROR;
}

// Every guard arm (and the `_ =>` fallback) must yield the same `comptime` type
auto cfg_evaluator::check_guard_arm_types(ast::node_id node, const ast::cfg_value_expr& expr)
    -> bool {
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

auto cfg_evaluator::atom_value_str(std::string_view atom) const -> std::string_view {
    if (atom == "os") { return cx_.facts.os; }
    if (atom == "arch") { return cx_.facts.arch; }
    if (atom == "abi") { return cx_.facts.abi; }
    if (atom == "family") { return cx_.facts.family; }
    if (atom == "optimize") { return optimize_mode_name(cx_.build_mode); }
    return cx_.facts.endian; // endian
}

auto cfg_evaluator::atom_value(std::string_view atom) -> cfg_value {
    PROFILE_FUNCTION();
    if (atom == "ptr_bits") { return cfg_value{static_cast<i64>(cx_.facts.ptr_bits)}; }
    if (atom == "safety") { return cfg_value{cx_.runtime_safety}; }
    if (atom == "testing") { return cfg_value{cx_.testing}; }
    return cfg_value{cfgval::member{atom_value_str(atom)}};
}

// `cpu.has(.feature)`
auto cfg_evaluator::is_cpu_has(const ast::call_expr& call) const -> bool {
    const auto dot{module_.ast.get_as_opt<ast::dot_expr>(call.function)};
    if (!dot || call.arguments.size() != 1) { return false; }
    const auto object{module_.ast.get_as_opt<ast::identifier_expr>(dot->object)};
    const auto member{module_.ast.get_as_opt<ast::identifier_expr>(dot->member)};
    return object && member && object->name == "cpu" && member->name == "has";
}

// True for an enabled feature of the target's architecture, false for a disabled one or one of
// another architecture; a name no architecture has is an error
auto cfg_evaluator::cpu_has(ast::node_id at, const ast::call_expr& call)
    -> stdx::option<cfg_value> {
    const auto arg{call.arguments.front().as_opt<ast::expr_handle>()};
    const auto access{arg ? module_.ast.get_as_opt<ast::implicit_access_expr>(*arg) : stdx::none};
    if (!access) {
        fail(at, error::CFG_ILLEGAL_PREDICATE, "cpu.has expects a feature, like `cpu.has(.avx2)`");
        return stdx::none;
    }
    const auto& name{module_.ast.get_as<ast::identifier_expr>(access->member).name};

    if (!target_cpu_) {
        auto resolved{codegen::resolve_target_cpu(cx_.target_opts)};
        if (!resolved) {
            fail(at,
                 error::CFG_ILLEGAL_PREDICATE,
                 "{}",
                 resolved.error().get_message().value_or("the target CPU can't be resolved"));
            return stdx::none;
        }
        target_cpu_.emplace(std::move(*resolved));
    }
    if (std::ranges::contains(target_cpu_->enabled, name)) { return cfg_value{true}; }

    std::string_view closest;
    usize            closest_dist{3}; // suggest only for distance <= 2
    for (const auto family : codegen::cpu_feature_families()) {
        if (std::ranges::contains(family->ghoti_names, name)) { return cfg_value{false}; }
        for (const auto& candidate : family->ghoti_names) {
            const auto dist{string_utils::edit_distance(name, candidate)};
            if (dist < closest_dist) {
                closest_dist = dist;
                closest      = candidate;
            }
        }
    }
    if (closest.empty()) {
        fail(at, error::CFG_UNKNOWN_ENUM_MEMBER, "no architecture has a CPU feature '{}'", name);
    } else {
        fail(at,
             error::CFG_UNKNOWN_ENUM_MEMBER,
             "no architecture has a CPU feature '{}'; did you mean '{}'?",
             name,
             closest);
    }
    return stdx::none;
}

auto cfg_evaluator::int_literal(ast::expr_handle h) -> stdx::option<i64> {
    PROFILE_FUNCTION();
    return module_.ast[h].visit([](const auto&) -> stdx::option<i64> { return stdx::none; },
                                [](const ast::int_literal_expr& e) -> stdx::option<i64> {
                                    return static_cast<i64>(e.value);
                                });
}

auto cfg_evaluator::eval_term(ast::expr_handle h) -> stdx::option<cfg_value> {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    switch (id.get_kind()) {
    case ast::node_kind::IDENTIFIER_EXPRESSION: {
        const auto& name{module_.ast.get_as<ast::identifier_expr>(id).name};
        if (is_cfg_atom(name)) { return atom_value(name); }
        if (const auto ref{cfg_value_decls_.find(name)}; ref != cfg_value_decls_.end()) {
            return resolve_cfg_value(ref->second.value);
        }
        fail(id,
             error::CFG_UNKNOWN_ATOM,
             "unknown cfg atom '{}'; valid: os, arch, abi, family, endian, ptr_bits, "
             "optimize, safety, testing, cpu.has(.feature) "
             "(or a @cfgValue constant)",
             name);
        return stdx::none;
    }
    case ast::node_kind::IMPLICIT_ACCESS_EXPRESSION: {
        const auto& access{module_.ast.get_as<ast::implicit_access_expr>(id)};
        return cfg_value{
            cfgval::member{module_.ast.get_as<ast::identifier_expr>(access.member).name}};
    }
    case ast::node_kind::DOT_EXPRESSION:       return resolve_foreign_value(id);
    case ast::node_kind::CFG_VALUE_EXPRESSION: return resolve_cfg_value(id);
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
        if (ast::node_id{call.function}.get_builtin() == syntax::builtin_t::COMPILE_ERROR) {
            fire_compile_error(id, call);
            return cfg_value{cfgval::diverges{}};
        }
        if (is_cpu_has(call)) { return cpu_has(id, call); }
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

auto cfg_evaluator::eval_predicate(ast::expr_handle h) -> stdx::option<bool> {
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

auto cfg_evaluator::classify_operand(ast::expr_handle h) -> operand {
    PROFILE_FUNCTION();
    const ast::node_id id{h};
    switch (id.get_kind()) {
    case ast::node_kind::IDENTIFIER_EXPRESSION: {
        const auto& name{module_.ast.get_as<ast::identifier_expr>(id).name};
        if (name == "ptr_bits") { return {.tag = operand::kind::PTR_BITS}; }
        if (name == "safety") {
            return {.tag = operand::kind::BOOL, .boolean = cx_.runtime_safety};
        }
        if (is_cfg_atom(name)) { return {.tag = operand::kind::ATOM, .text = name}; }
        if (const auto ref{cfg_value_decls_.find(name)}; ref != cfg_value_decls_.end()) {
            const auto value{resolve_cfg_value(ref->second.value)};
            if (!value) { return {}; } // error already reported
            return to_operand(*value);
        }
        fail(id,
             error::CFG_UNKNOWN_ATOM,
             "unknown cfg atom '{}'; valid: os, arch, abi, family, endian, ptr_bits, "
             "optimize, safety, testing, cpu.has(.feature) "
             "(or a @cfgValue constant)",
             name);
        return {};
    }
    case ast::node_kind::DOT_EXPRESSION: {
        const auto value{resolve_foreign_value(id)};
        if (!value) { return {}; } // error already reported
        return to_operand(*value);
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

auto cfg_evaluator::eval_comparison(ast::node_id     at,
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
        const i64 l{static_cast<i64>(cx_.facts.ptr_bits)};
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

auto cfg_evaluator::compile_error_message(const ast::call_expr& call)
    -> stdx::option<std::string_view> {
    PROFILE_FUNCTION();
    if (call.arguments.empty()) { return stdx::none; }
    const auto arg{call.arguments.front().as_opt<ast::expr_handle>()};
    if (!arg) { return stdx::none; }
    if (const auto str{module_.ast.get_as_opt<ast::string_expr>(*arg)}) { return str->value; }
    if (const auto ident{module_.ast.get_as_opt<ast::identifier_expr>(*arg)}) {
        if (const auto ref{cfg_value_decls_.find(ident->name)}; ref != cfg_value_decls_.end()) {
            if (const auto value{resolve_cfg_value(ref->second.value)}) {
                if (const auto str{value->as_opt<cfgval::text>()}) { return str->value; }
            }
        }
    }
    return stdx::none;
}

auto cfg_evaluator::fire_compile_error(ast::node_id at, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    ok_ = false;
    if (const auto message{compile_error_message(call)}) {
        return diags_.emplace_back(
            std::string{*message}, error::COMPILE_ERROR_REACHED, module_.ast.location_of(at));
    }

    diags_.emplace_back(
        "@compileError requires a compile-time-known string message (a string literal or a "
        "@cfgValue string constant)",
        error::COMPILE_ERROR_NON_CONSTANT_MESSAGE,
        module_.ast.location_of(at));
}

auto cfg_evaluator::fire_eager_compile_errors(const std::vector<ast::stmt_handle>& items) -> void {
    PROFILE_FUNCTION();
    for (const auto item : items) {
        const auto expr_stmt{module_.ast.get_as_opt<ast::expr_stmt>(item)};
        if (!expr_stmt) { continue; }
        const auto call{module_.ast.get_as_opt<ast::call_expr>(expr_stmt->expression)};
        if (call &&
            ast::node_id{call->function}.get_builtin() == syntax::builtin_t::COMPILE_ERROR) {
            fire_compile_error(ast::node_id{expr_stmt->expression}, *call);
        }
    }
}

} // namespace ghoti::sema
