#include <algorithm>
#include <concepts>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/sema/alias_chain.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// The return kind of a callable declaration's type; `AUTO` when a builtin's return is unknown
[[nodiscard]] auto callable_return_kind(const type::data_t& data) -> stdx::option<type_kind> {
    if (data.is<types::builtin_function>()) { return type_kind::AUTO; }
    if (const auto fn{data.as_opt<types::function>()}) { return fn->return_type.get_kind(); }
    if (const auto closure{data.as_opt<types::closure_t>()}) {
        if (const auto sig{closure->signature.get_data().as_opt<types::function>()}) {
            return sig->return_type.get_kind();
        }
    }
    return stdx::none;
}

// An expression whose value never needs binding: an assignment's value already landed, an
// unused `if`/`match` has its arms checked one by one instead, and a type or function literal
// has no runtime value to drop
[[nodiscard]] auto is_statement_form(const ast::AST& tree, ast::expr_handle expr) -> bool {
    if (const auto cx{tree.get_as_opt<ast::comptime_expr>(expr)}) {
        return is_statement_form(tree, cx->rhs);
    }
    switch (ast::node_id{expr}.get_kind()) {
    case ast::node_kind::ASSIGNMENT_EXPRESSION:
    case ast::node_kind::IF_EXPRESSION:
    case ast::node_kind::MATCH_EXPRESSION:
    case ast::node_kind::WHILE_LOOP_EXPRESSION:
    case ast::node_kind::DO_WHILE_LOOP_EXPRESSION:
    case ast::node_kind::FOR_LOOP_EXPRESSION:
    case ast::node_kind::INFINITE_LOOP_EXPRESSION:
    case ast::node_kind::LABEL_EXPRESSION:
    case ast::node_kind::FUNCTION_EXPRESSION:
    case ast::node_kind::STRUCT_EXPRESSION:
    case ast::node_kind::UNION_EXPRESSION:
    case ast::node_kind::ENUM_EXPRESSION:
    case ast::node_kind::INTERFACE_EXPRESSION:     return true;
    default:                                       return false;
    }
}

} // namespace

auto type_resolver::fold_attribute_bool(const ast::attribute& item) -> stdx::option<bool> {
    if (item.args.empty()) { return true; }
    const auto arg{item.args.front()};
    resolve(arg);
    gir::const_eval evaluator{ctx_, resolving_};
    const auto      cv{evaluator.try_eval(arg)};
    if (const auto folded{cv ? cv->as_opt<bool>() : stdx::none}) { return *folded; }
    ctx_.diags.emplace_back(fmt::format("Attribute '{}' requires a compile-time 'bool' argument",
                                        ast::attribute_spec_of(item.kind).name),
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of(arg));
    return stdx::none;
}

auto type_resolver::hint_site_of(const ast::block_stmt& block) -> stdx::option<hint_site> {
    if (!hint_sites_built_) {
        hint_sites_built_ = true;
        const auto& tree{resolving_.ast};
        const auto  add_branch{[&](ast::stmt_handle stmt) {
            if (const auto branch{tree.get_as_opt<ast::block_stmt>(*stmt)}) {
                hint_sites_.try_emplace(branch.get(), hint_site::BRANCH);
            }
        }};
        for (const auto id : tree.nodes_of<ast::if_expr>()) {
            const auto& if_expr{tree.get_as<ast::if_expr>(id)};
            add_branch(if_expr.consequence);
            if (if_expr.alternate) { add_branch(*if_expr.alternate); }
        }
        for (const auto id : tree.nodes_of<ast::match_expr>()) {
            for (const auto& arm : tree.get_as<ast::match_expr>(id).arms) {
                add_branch(arm.dispatch);
            }
        }
        for (const auto id : tree.nodes_of<ast::function_expr>()) {
            const auto& fn{tree.get_as<ast::function_expr>(id)};
            if (fn.body) {
                hint_sites_.try_emplace(&tree.get_as<ast::block_stmt>(*fn.body),
                                        hint_site::FN_BODY);
            }
        }
    }
    const auto it{hint_sites_.find(&block)};
    if (it == hint_sites_.end()) { return stdx::none; }
    return it->second;
}

auto type_resolver::scoped_runtime_safety() const -> bool {
    for (const auto& frame : active_blocks_ | std::views::reverse) {
        if (!frame.runtime_safety) { continue; }
        // A setting in an enclosing function's body does not reach into this one
        if (frame.safety_fn_depth != function_boundaries_.size()) { break; }
        return *frame.runtime_safety;
    }
    return ctx_.runtime_safety;
}

auto type_resolver::check_deprecation_message(const ast::attribute& item) -> void {
    if (item.args.empty() || resolving_.ast.get_as_opt<ast::string_expr>(item.args.front())) {
        return;
    }
    ctx_.diags.emplace_back("Attribute 'deprecated' takes a string literal message",
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of(item.args.front()));
}

namespace {

// The `deprecated` entry of whatever `sym` declares, with the module whose AST holds it
[[nodiscard]] auto deprecation_of(const mod::module_t& owner, const symbol& sym)
    -> stdx::option<const ast::attribute&> {
    const auto find{[](const stdx::option<ast::attribute_list>& attributes)
                        -> stdx::option<const ast::attribute&> {
        return attributes ? attributes->find(ast::attribute_kind::DEPRECATED) : stdx::none;
    }};
    return sym.get_data().visit(
        [&](const symbols::node_t& node) -> stdx::option<const ast::attribute&> {
            const auto decl{owner.ast.get_as_opt<ast::decl_stmt>(node)};
            return decl ? find(decl->attributes) : stdx::none;
        },
        [&](const symbols::struct_field& field) { return find(field.attributes); },
        [&](const symbols::union_field& field) { return find(field.attributes); },
        [](const auto&) -> stdx::option<const ast::attribute&> { return stdx::none; });
}

} // namespace

template <ast::IndexableID ID>
auto type_resolver::report_deprecated_use(ID id, const mod::module_t& owner, const symbol& sym)
    -> void {
    if (deprecated_scope_depth_ > 0 || ctx_.deprecated_policy == deprecation_policy::ALLOW) {
        return;
    }
    const auto deprecation{deprecation_of(owner, sym)};
    if (!deprecation) { return; }

    const auto& loc{resolving_.ast.location_of(id)};
    const auto  site{fmt::format("{}:{}:{}", resolving_.path.string(), loc.line, loc.column)};
    if (!ctx_.reported_deprecations.insert(site).second) { return; }

    std::string message{fmt::format("'{}' is deprecated", sym.get_name())};
    // A message that isn't a string literal was already reported at the attribute itself
    if (const auto note{deprecation->args.empty()
                            ? stdx::none
                            : owner.ast.get_as_opt<ast::string_expr>(deprecation->args.front())}) {
        message += fmt::format(": {}", note->value);
    }
    if (ctx_.deprecated_policy == deprecation_policy::DENY) {
        ctx_.diags.emplace_back(std::move(message), error::DEPRECATED_USE, loc);
        return;
    }
    diagnostic warning{std::move(message), error::DEPRECATED_USE, loc};
    warning.set_level(diagnostic_level::WARNING);
    resolving_.warnings.emplace_back(warning);
}

template auto type_resolver::report_deprecated_use<ast::node_id>(ast::node_id,
                                                                 const mod::module_t&,
                                                                 const symbol&) -> void;
template auto type_resolver::report_deprecated_use<ast::explicit_type_id>(ast::explicit_type_id,
                                                                          const mod::module_t&,
                                                                          const symbol&) -> void;

auto type_resolver::in_testing_context() const -> bool {
    if (open_function_nodes_.empty()) { return instantiating_testing_fn_; }
    const auto innermost{open_function_nodes_.back()};
    return innermost.get_kind() == ast::node_kind::TEST_STATEMENT ||
           declares_testing_fn(resolving_, innermost);
}

template <ast::IndexableID ID>
auto type_resolver::check_testing_use(ID id, const mod::module_t& owner, const symbol& sym)
    -> void {
    // A type position can't name a function to call
    if constexpr (!std::same_as<ID, ast::node_id>) { return; }
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node || !declares_testing_fn(owner, ast::node_id{*node})) { return; }
    const auto loc{resolving_.ast.location_of(id)};
    if (!direct_callee_ || *direct_callee_ != id.get_index()) {
        ctx_.diags.emplace_back(
            fmt::format("'{}' is a `@[testing]` function, which can only be called directly",
                        sym.get_name()),
            error::TESTING_FN_OUTSIDE_TEST,
            loc);
        return;
    }
    if (!in_testing_context()) {
        ctx_.diags.emplace_back(
            fmt::format("'{}' is a `@[testing]` function, so only a test or another "
                        "`@[testing]` function may call it",
                        sym.get_name()),
            error::TESTING_FN_OUTSIDE_TEST,
            loc);
    }
}

template auto type_resolver::check_testing_use<ast::node_id>(ast::node_id,
                                                             const mod::module_t&,
                                                             const symbol&) -> void;
template auto type_resolver::check_testing_use<ast::explicit_type_id>(ast::explicit_type_id,
                                                                      const mod::module_t&,
                                                                      const symbol&) -> void;

auto type_resolver::fold_alignment(ast::expr_handle arg) -> stdx::option<u64> {
    resolve(arg);
    gir::const_eval evaluator{ctx_, resolving_};
    const auto      cv{evaluator.try_eval(arg)};
    const auto      value{cv ? cv->as_uint_opt() : stdx::none};
    const auto      is_power_of_two{value && *value > 0 && (*value & (*value - 1)) == 0};
    if (is_power_of_two && *value <= std::numeric_limits<u32>::max()) {
        return static_cast<u64>(*value);
    }
    ctx_.diags.emplace_back("Attribute 'align' requires a compile-time power-of-two integer",
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of(arg));
    return stdx::none;
}

auto type_resolver::resolve_field_attributes(const stdx::option<ast::attribute_list>& attributes)
    -> stdx::option<u64> {
    if (!attributes) { return stdx::none; }
    stdx::option<u64> alignment;
    for (const auto& item : attributes->items) {
        const auto& spec{ast::attribute_spec_of(item.kind)};
        if (!static_cast<bool>(spec.targets & ast::attribute_target::FIELD)) {
            ctx_.diags.emplace_back(
                fmt::format("Attribute '{}' cannot be applied to a field", spec.name),
                error::ILLEGAL_ATTRIBUTE,
                resolving_.ast.location_of(item.name));
            continue;
        }
        if (item.kind == ast::attribute_kind::ALIGN) {
            alignment = fold_alignment(item.args.front());
        } else if (item.kind == ast::attribute_kind::DEPRECATED) {
            check_deprecation_message(item);
        }
    }
    return alignment;
}

auto type_resolver::record_export(const ast::call_expr& call) -> stdx::option<diagnostic> {
    const auto& target_arg{call.arguments[0]};
    const auto  target_expr{target_arg.as_opt<ast::expr_handle>()};
    const auto  target_loc{get_call_arg_location(target_arg)};
    const auto  not_a_function{[&] {
        return diagnostic{"'@export' expects a function declared with 'const', like "
                          "'@export(add, .{ .name = \"add\" })'",
                          error::TYPE_MISMATCH,
                          target_loc};
    }};
    if (!target_expr) { return not_a_function(); }

    // The function the argument names: a declaration written out, or a function passed to a
    // `comptime` parameter (a helper in the style of Zig's `symbol`)
    stdx::option<const mod::module_t&>  owner;
    stdx::option<ast::node_id>          fn_node;
    stdx::option<const ast::decl_stmt&> decl;
    ast::node_id                        named{*target_expr};
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(named)}) { named = dot->member; }
    if (const auto declared{resolving_.get_identifier_declaration(named)};
        declared && declared->owner && declared->decl) {
        const auto found{declared->owner->ast.get_as_opt<ast::decl_stmt>(*declared->decl)};
        if (found && found->value && !found->has_modifier(ast::decl_modifiers::MUT)) {
            owner.emplace(*declared->owner);
            fn_node.emplace(*found->value);
            decl.emplace(*found);
        }
    } else {
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      value{evaluator.try_eval(*target_expr)};
        if (const auto closure{value ? value->as_opt<gir::const_closure>() : stdx::none};
            closure && closure->captures.fields.empty()) {
            owner.emplace(closure->module ? *closure->module : resolving_);
            fn_node.emplace(closure->fn_node);
        }
    }
    if (!owner || !fn_node || !owner->ast.get_as_opt<ast::function_expr>(*fn_node)) {
        return not_a_function();
    }

    const auto fn_type{owner->get_sema_type_opt(*fn_node)};
    if (fn_type && ctx_.generic_functions.get_opt(*fn_type)) {
        return diagnostic{"'@export' can't export a generic function; export a non-generic "
                          "wrapper that calls one instantiation",
                          error::TYPE_MISMATCH,
                          target_loc};
    }
    if (fn_type && sema::signature_of(*fn_type) &&
        sema::signature_of(*fn_type)->return_type.get_kind() == type_kind::TYPE) {
        return diagnostic{"'@export' can't export a function that returns a type; it only exists "
                          "at compile time",
                          error::TYPE_MISMATCH,
                          target_loc};
    }
    if (fn_type) {
        if (auto problem{unmodeled_c_abi_error(*fn_type, target_loc)}) { return problem; }
    }
    const auto fn_name{decl ? owner->ast.get_as<ast::identifier_expr>(decl->name).name
                            : std::string_view{}};
    if (decl && decl->has_modifier(ast::decl_modifiers::EXPORT)) {
        return diagnostic{fmt::format("'{}' is already exported by its declaration; give every "
                                      "name it needs with '@export' instead",
                                      fn_name),
                          error::TYPE_MISMATCH,
                          target_loc};
    }

    // The options fold to a `builtin.ExportOptions` value
    const auto&                    options_arg{call.arguments[1]};
    const auto                     options_loc{get_call_arg_location(options_arg)};
    const auto                     options_expr{options_arg.as_opt<ast::expr_handle>()};
    stdx::option<gir::const_value> folded;
    if (options_expr) {
        gir::const_eval evaluator{ctx_, resolving_};
        folded = evaluator.try_eval(*options_expr);
    }
    const auto options{folded ? folded->as_opt<gir::const_struct>() : stdx::none};
    const auto name_value{options ? options->get_field_opt("name") : stdx::none};
    const auto name{name_value ? name_value->as_opt<std::string>() : stdx::none};
    if (!name) {
        return diagnostic{"'@export' options must be a compile-time 'builtin.ExportOptions' with "
                          "a 'name'",
                          error::TYPE_MISMATCH,
                          options_loc};
    }
    if (name->empty() || name->contains('\0')) {
        return diagnostic{"An exported symbol name can't be empty or hold a zero byte",
                          error::TYPE_MISMATCH,
                          options_loc};
    }
    const auto enum_field{[&](std::string_view field) -> std::string {
        const auto value{options->get_field_opt(field)};
        const auto variant{value ? value->as_opt<gir::const_enum>() : stdx::none};
        return variant ? variant->name : std::string{};
    }};
    const auto visibility{ast::symbol_visibility_from_name(enum_field("visibility"))
                              .value_or(ast::symbol_visibility::DEFAULT)};
    if (visibility == ast::symbol_visibility::PROTECTED &&
        !codegen::resolve_target_triple(ctx_.target_opts.triple_str).isOSBinFormatELF()) {
        return diagnostic{
            "Visibility '.protected' needs an ELF target", error::TYPE_MISMATCH, options_loc};
    }

    // Exporting a function as `main` makes it the program's entry point
    if (*name == "main" && !resolving_.is_start) {
        if (!decl || owner != resolving_) {
            return diagnostic{"A function exported as 'main' must be declared in the module that "
                              "exports it, and named directly",
                              error::TYPE_MISMATCH,
                              target_loc};
        }
        const auto declared_main{resolving_.root_table_idx ? ctx_.registry.get_from_opt(
                                                                 *resolving_.root_table_idx, "main")
                                                           : stdx::none};
        if (declared_main && fn_name != "main") {
            return diagnostic{fmt::format("'{}' can't be exported as 'main' because this module "
                                          "already declares 'main'",
                                          fn_name),
                              error::TYPE_MISMATCH,
                              target_loc};
        }
        ctx_.user_main_name = std::string{fn_name};
    }

    if (const auto earlier{ctx_.exports.named(*name)}) {
        // Resolving the same `@export` again (a generic body, a re-resolution) is no conflict
        const bool same{earlier->owner && earlier->owner == owner &&
                        earlier->fn_node.get_index() == fn_node->get_index()};
        if (same) { return stdx::none; }
        // The entry code's C `main` stands in for the program's `main` export
        const bool entry_main{*name == "main" && (resolving_.is_start ||
                                                  (earlier->owner && earlier->owner->is_start))};
        if (entry_main) {
            ctx_.exports.add({
                .owner      = *owner,
                .fn_node    = *fn_node,
                .name       = *name,
                .weak       = enum_field("linkage") == "weak",
                .visibility = visibility,
                .site       = resolving_.ast.location_of(call.function),
            });
            return stdx::none;
        }
        return diagnostic{fmt::format("The symbol '{}' is already exported at {}:{}",
                                      *name,
                                      earlier->site.line + 1,
                                      earlier->site.column + 1),
                          error::TYPE_MISMATCH,
                          options_loc};
    }
    ctx_.exports.add({
        .owner      = *owner,
        .fn_node    = *fn_node,
        .name       = *name,
        .weak       = enum_field("linkage") == "weak",
        .visibility = visibility,
        .site       = resolving_.ast.location_of(call.function),
    });
    return stdx::none;
}

auto type_resolver::fold_attribute_enum(const ast::attribute& item, std::string_view enum_name)
    -> stdx::option<std::string> {
    const auto arg{item.args.front()};
    auto&      enum_type{ctx_.get_builtin_type(enum_name)};
    {
        const structural_guard g{implicit_type_stack_, enum_type};
        resolve(arg);
    }

    const auto arg_type{resolving_.get_sema_type_opt(arg)};
    if (arg_type && arg_type.get() == &enum_type) {
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      cv{evaluator.try_eval(arg)};
        if (const auto variant{cv ? cv->as_opt<gir::const_enum>() : stdx::none}) {
            return variant->name;
        }
    }
    ctx_.diags.emplace_back(fmt::format("Attribute '{}' requires a compile-time 'builtin.{}' value",
                                        ast::attribute_spec_of(item.kind).name,
                                        enum_name),
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of(arg));
    return stdx::none;
}

// The type of the root module's `main`, for `@entryMain()`
auto type_resolver::entry_main_type() const -> stdx::option<type&> {
    if (!ctx_.entry_root || !ctx_.entry_root->root_table_idx) { return stdx::none; }
    const auto sym{
        ctx_.registry.get_from_opt(*ctx_.entry_root->root_table_idx, ctx_.user_main_name)};
    if (!sym || sym->get_status() != symbol_status::RESOLVED) { return stdx::none; }
    const auto node{sym->get_data().as_opt<symbols::node_t>()};
    const auto decl{node ? ctx_.entry_root->ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
    if (!decl || !decl->value) { return stdx::none; }
    const auto fn_type{ctx_.entry_root->get_sema_type_opt(*decl->value)};
    if (!fn_type || fn_type->get_kind() != type_kind::FUNCTION) { return stdx::none; }
    return *fn_type;
}

auto type_resolver::entry_test_runner_type() const -> stdx::option<type&> {
    const auto owner{ctx_.entry_test_runner_module()};
    if (!owner || !owner->root_table_idx) { return stdx::none; }
    const auto sym{ctx_.registry.get_from_opt(*owner->root_table_idx, "test_runner")};
    if (!sym || sym->get_status() != symbol_status::RESOLVED) { return stdx::none; }
    const auto node{sym->get_data().as_opt<symbols::node_t>()};
    const auto runner_type{node ? owner->get_sema_type_opt(*node) : stdx::none};
    if (!runner_type || runner_type->get_kind() != type_kind::FUNCTION) { return stdx::none; }
    return *runner_type;
}

// Each `builtin.CpuFeature` argument, as the `+name` LLVM enables it by
auto type_resolver::fold_target_features(const ast::attribute& item) -> std::vector<std::string> {
    const auto triple{codegen::resolve_target_triple(ctx_.target_opts.triple_str)};
    const auto family{codegen::cpu_feature_family_of(triple)};
    auto&      feature_type{denoted_type(ctx_.get_builtin_type("CpuFeature"))};

    std::vector<std::string> features;
    for (const auto arg : item.args) {
        {
            const structural_guard g{implicit_type_stack_, feature_type};
            resolve(arg);
        }
        const auto arg_type{resolving_.get_sema_type_opt(arg)};
        if (arg_type && arg_type->is_poison()) { continue; }
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      cv{arg_type && is_same_unqualified(*arg_type, feature_type)
                               ? evaluator.try_eval(arg)
                               : stdx::none};
        const auto      variant{cv ? cv->as_opt<gir::const_enum>() : stdx::none};
        const auto      at{variant && family ? std::ranges::find(family->ghoti_names, variant->name)
                                             : std::vector<std::string>::const_iterator{}};
        if (!variant || !family || at == family->ghoti_names.end()) {
            ctx_.diags.emplace_back("Attribute 'target_features' requires compile-time "
                                    "'builtin.CpuFeature' values",
                                    error::ILLEGAL_ATTRIBUTE,
                                    resolving_.ast.location_of(arg));
            continue;
        }
        const auto index{static_cast<usize>(at - family->ghoti_names.begin())};
        features.emplace_back(fmt::format("+{}", family->llvm_names[index]));
    }
    return features;
}

auto type_resolver::fold_visibility(const ast::attribute& item)
    -> stdx::option<ast::symbol_visibility> {
    const auto variant{fold_attribute_enum(item, "Visibility")};
    const auto visibility{variant ? ast::symbol_visibility_from_name(*variant) : stdx::none};
    if (visibility != ast::symbol_visibility::PROTECTED) { return visibility; }

    const auto triple{codegen::resolve_target_triple(ctx_.target_opts.triple_str)};
    if (triple.isOSBinFormatELF()) { return visibility; }
    ctx_.diags.emplace_back(fmt::format("Visibility '.protected' needs an ELF target; '{}' has no "
                                        "protected symbols",
                                        triple.str()),
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of(item.args.front()));
    return stdx::none;
}

auto type_resolver::check_visibility_linkage(const attribute_refs&               items,
                                             stdx::option<const ast::decl_stmt&> decl) -> void {
    const auto item{std::ranges::find_if(
        items, [](const auto attr) { return attr->kind == ast::attribute_kind::VISIBILITY; })};
    if (item == items.end()) { return; }

    using enum ast::decl_modifiers;
    const bool linked{decl && (decl->has_modifier(PUBLIC) || decl->has_modifier(EXPORT) ||
                               decl->has_modifier(EXTERN) || decl->has_modifier(WEAK))};
    if (linked) { return; }
    ctx_.diags.emplace_back("Attribute 'visibility' needs a symbol other objects can link to: a "
                            "'pub', 'export', 'extern', or 'weak' declaration",
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of((*item)->name));
}

auto type_resolver::check_attribute_conflicts(const attribute_refs&      items,
                                              const resolved_attributes& resolved) -> void {
    if (!resolved.naked || resolved.inlining != ast::inline_mode::ALWAYS) { return; }
    const auto inline_item{std::ranges::find_if(
        items, [](const auto item) { return item->kind == ast::attribute_kind::INLINE; })};
    ctx_.diags.emplace_back("A 'naked' function cannot be 'inline(.always)'",
                            error::ILLEGAL_ATTRIBUTE,
                            resolving_.ast.location_of((*inline_item)->name));
}

auto type_resolver::resolve_attributes(const attribute_refs& items,
                                       ast::attribute_target site,
                                       bool                  returns_void) -> resolved_attributes {
    resolved_attributes resolved;
    for (const auto item : items) {
        const auto& spec{ast::attribute_spec_of(item->kind)};
        if (!static_cast<bool>(spec.targets & site)) {
            const auto* what{ast::routes_to_fn_literal(item->kind)
                                 ? "a declaration whose initializer is not a function literal"
                                 : "a non-function declaration"};
            ctx_.diags.emplace_back(
                fmt::format("Attribute '{}' cannot be applied to {}", spec.name, what),
                error::ILLEGAL_ATTRIBUTE,
                resolving_.ast.location_of(item->name));
            continue;
        }

        switch (item->kind) {
        case ast::attribute_kind::DISCARDABLE:
            if (returns_void) {
                ctx_.diags.emplace_back(
                    "Attribute 'discardable' has no effect on a function that returns 'void'",
                    error::ILLEGAL_ATTRIBUTE,
                    resolving_.ast.location_of(item->name));
            }
            resolved.discardable = fold_attribute_bool(*item).value_or(false);
            break;
        case ast::attribute_kind::INLINE:
            if (const auto variant{fold_attribute_enum(*item, "Inline")}) {
                const auto mode{ast::inline_mode_from_name(*variant)};
                if (mode != ast::inline_mode::DEFAULT) { resolved.inlining = mode; }
            }
            break;
        case ast::attribute_kind::NAKED:
            resolved.naked = fold_attribute_bool(*item).value_or(false);
            break;
        case ast::attribute_kind::ALIGN:
            resolved.alignment = fold_alignment(item->args.front());
            break;
        case ast::attribute_kind::DEPRECATED: check_deprecation_message(*item); break;
        case ast::attribute_kind::VISIBILITY: resolved.visibility = fold_visibility(*item); break;
        case ast::attribute_kind::TESTING:    resolved.testing = true; break;
        case ast::attribute_kind::NO_BUILTINS:
            resolved.no_builtins = fold_attribute_bool(*item).value_or(false);
            break;
        case ast::attribute_kind::TARGET_FEATURES:
            resolved.target_features = fold_target_features(*item);
            break;
        }
    }

    check_attribute_conflicts(items, resolved);
    if (resolved.naked && !resolved.inlining) { resolved.inlining = ast::inline_mode::NEVER; }
    return resolved;
}

auto type_resolver::resolve_decl_attributes(ast::node_id          id,
                                            const ast::decl_stmt& decl,
                                            const type::data_t&   type_data) -> void {
    if (!decl.attributes) { return; }
    const bool initializes_fn_literal{decl.value && decl.value->is<ast::function_expr>()};

    // Function-only attributes belong to the literal and resolve with it instead
    attribute_refs own;
    for (const auto& item : decl.attributes->items) {
        if (initializes_fn_literal && ast::routes_to_fn_literal(item.kind)) { continue; }
        own.emplace_back(&item);
    }

    check_visibility_linkage(own, decl);
    const auto return_kind{callable_return_kind(type_data)};
    resolving_.set_node_attributes(id,
                                   resolve_attributes(own,
                                                      return_kind ? ast::attribute_target::FN_DECL
                                                                  : ast::attribute_target::DECL,
                                                      return_kind == type_kind::VOID_));

    const auto align_item{decl.attributes->find(ast::attribute_kind::ALIGN)};
    if (align_item && !initializes_fn_literal && decl.has_modifier(ast::decl_modifiers::COMPTIME)) {
        ctx_.diags.emplace_back("Attribute 'align' needs storage; a 'const' declaration has "
                                "none",
                                error::ILLEGAL_ATTRIBUTE,
                                resolving_.ast.location_of(align_item->name));
    }
}

auto type_resolver::mentions_unbound_type(const ast::call_expr::argument& arg) const -> bool {
    const auto expr{arg.as_opt<ast::expr_handle>()};
    if (!expr) { return false; }
    const auto& tree{resolving_.ast};
    const auto position{[](const source_location& loc) { return std::pair{loc.line, loc.column}; }};
    const auto start{position(tree.location_of(*expr))};
    const auto end{position(tree.end_location_of(*expr))};
    for (const auto id : tree.nodes_of<ast::identifier_expr>()) {
        const auto at{position(tree.location_of(id))};
        if (at < start || at >= end) { continue; }
        const auto t{resolving_.get_sema_type_opt(id)};
        if (t && t->get_kind() == type_kind::TYPE && !t->get_data().is<types::meta_type>()) {
            return true;
        }
    }
    return false;
}

auto type_resolver::mentions_fn_param(const ast::attribute&     item,
                                      const ast::function_expr& fn) const -> bool {
    const auto&                   tree{resolving_.ast};
    std::vector<std::string_view> params;
    for (const auto& p : fn.parameters) {
        if (const auto name{tree.get_as_opt<ast::identifier_expr>(p.name)}) {
            params.emplace_back(name->name);
        }
    }
    if (params.empty()) { return false; }

    // Any identifier spelled inside an argument's span, so a member named like a parameter counts
    const auto position{[](const source_location& loc) { return std::pair{loc.line, loc.column}; }};
    for (const auto arg : item.args) {
        const auto start{position(tree.location_of(arg))};
        const auto end{position(tree.end_location_of(arg))};
        for (const auto id : tree.nodes_of<ast::identifier_expr>()) {
            const auto at{position(tree.location_of(id))};
            if (at < start || at >= end) { continue; }
            if (std::ranges::contains(params, tree.get_as<ast::identifier_expr>(id).name)) {
                return true;
            }
        }
    }
    return false;
}

auto type_resolver::resolve_fn_literal_attributes(ast::node_id              id,
                                                  const ast::function_expr& fn,
                                                  bool                      is_template) -> void {
    // Inside a monomorph every visit refolds, since the arguments may depend on its parameters
    if (!for_generic_instantiation_ && resolving_.attributes_of(id)) { return; }

    attribute_refs items;
    if (fn.attributes) {
        for (const auto& item : fn.attributes->items) { items.emplace_back(&item); }
    }
    if (fn.declaring_decl) {
        const auto& decl{resolving_.ast.get_as<ast::decl_stmt>(*fn.declaring_decl)};
        if (decl.attributes) {
            for (const auto& item : decl.attributes->items) {
                if (!ast::routes_to_fn_literal(item.kind)) { continue; }
                if (fn.attributes && fn.attributes->find(item.kind)) {
                    ctx_.diags.emplace_back(
                        fmt::format("Attribute '{}' is applied to both the declaration and its "
                                    "function literal",
                                    ast::attribute_spec_of(item.kind).name),
                        error::ILLEGAL_ATTRIBUTE,
                        resolving_.ast.location_of(item.name));
                    continue;
                }
                items.emplace_back(&item);
            }
        }
    }
    if (items.empty()) { return; }

    stdx::option<ast::attribute_kind> dependent;
    if (is_template) {
        std::erase_if(items, [&](const auto item) {
            if (!mentions_fn_param(*item, fn)) { return false; }
            if (!dependent) { dependent = item->kind; }
            return true;
        });
    }

    const bool                          returns_void{fn.explicit_return_type.get_token_type() ==
                                                     syntax::token_type_t::VOID_TYPE};
    stdx::option<const ast::decl_stmt&> declaring;
    if (fn.declaring_decl) {
        declaring.emplace(resolving_.ast.get_as<ast::decl_stmt>(*fn.declaring_decl));
    }
    check_visibility_linkage(items, declaring);
    auto resolved{resolve_attributes(items, ast::attribute_target::FN, returns_void)};
    resolved.dependent = dependent;
    resolving_.set_node_attributes(id, resolved);
}

auto type_resolver::discardable_holds(const mod::module_t&                     home,
                                      ast::node_id                             owner,
                                      const stdx::option<ast::attribute_list>& attributes,
                                      ast::node_id call_id) const -> stdx::option<bool> {
    if (!attributes) { return stdx::none; }
    const auto item{attributes->find(ast::attribute_kind::DISCARDABLE)};
    if (!item) { return stdx::none; }
    if (item->args.empty()) { return true; }

    if (const auto target{resolving_.get_generic_call_target_opt(call_id)}) {
        if (const auto diff{ctx_.instantiation_cache.get_body_type_diff(*target)}) {
            if (const auto resolved{diff->find_attributes(owner.get_index())}) {
                return resolved->discardable;
            }
        }
    }
    const auto resolved{home.attributes_of(owner)};
    return resolved && resolved->discardable;
}

auto type_resolver::interface_member_discardable(const type&      interface_type,
                                                 std::string_view name,
                                                 ast::node_id call_id) const -> stdx::option<bool> {
    const auto iface{interface_type.get_data().as_opt<types::interface_t>()};
    if (!iface) { return stdx::none; }
    for (const auto& method : iface->ast_methods) {
        if (iface->enclosing.ast.get_as<ast::identifier_expr>(*method.name).name != name) {
            continue;
        }
        return discardable_holds(iface->enclosing, *method.signature, method.attributes, call_id);
    }
    return stdx::none;
}

auto type_resolver::impl_method_discardable(const type&      target,
                                            std::string_view name,
                                            ast::node_id     call_id) const -> bool {
    const auto same_target{[&](const impl_record& r) {
        return r.target_type && r.target_type->has_symbol_table_idx() &&
               r.target_type->get_symbol_table_idx() == target.get_symbol_table_idx();
    }};
    for (const auto* record : ctx_.impls.records()) {
        if (!same_target(*record)) { continue; }
        const auto method{record->find_method(name)};
        if (!method) { continue; }

        const auto defining{method->defining_mod ? method->defining_mod : record->enclosing};
        if (defining && !method->inherited) {
            if (const auto decl{defining->ast.get_as_opt<ast::decl_stmt>(method->decl)}) {
                const auto owner{defining->fn_literal_node(method->decl)};
                const auto literal{defining->ast.get_as_opt<ast::function_expr>(owner)};
                if (const auto holds{
                        discardable_holds(*defining, owner, decl->attributes, call_id)}) {
                    return *holds;
                }
                if (literal) {
                    if (const auto holds{
                            discardable_holds(*defining, owner, literal->attributes, call_id)}) {
                        return *holds;
                    }
                }
            }
        }
        if (record->interface_type) {
            return interface_member_discardable(*record->interface_type, name, call_id)
                .value_or(false);
        }
        return false;
    }
    return false;
}

auto type_resolver::callee_is_discardable(ast::node_id call_id, const ast::call_expr& call) const
    -> bool {
    stdx::option<const mod::module_t&> home{resolving_};
    ast::node_id                       fn_node{*call.function};

    alias_chain chain;
    while (chain.step(*home, fn_node)) {
        stdx::option<symbol&> sym;
        if (const auto ident{home->ast.get_as_opt<ast::identifier_expr>(fn_node)}) {
            sym = (home == &resolving_) ? ctx_.registry.lookup(table_stack_, ident->name)
                                        : stdx::option<symbol&>{};
            if (!sym && home->root_table_idx) {
                sym = ctx_.registry.get_from_opt(*home->root_table_idx, ident->name);
            }
        } else if (const auto dot{home->ast.get_as_opt<ast::dot_expr>(fn_node)}) {
            const auto outer_type{home->get_sema_type_opt(dot->object)};
            if (!outer_type) { return false; }
            if (const auto m_data{outer_type->get_data().as_opt<types::module_t>()}) {
                if (!m_data->imported.root_table_idx) { return false; }
                const auto& inner_ident{home->ast.get_as<ast::identifier_expr>(dot->member)};
                sym =
                    ctx_.registry.get_from_opt(*m_data->imported.root_table_idx, inner_ident.name);
                home.emplace(m_data->imported);
            } else {
                const auto unwrap_ref = [](const type& t) -> const type& {
                    const type* curr{&t};
                    if (const auto meta{curr->get_data().as_opt<types::meta_type>()}) {
                        curr = &meta->instance;
                    }
                    if (const auto ptr{curr->get_data().as_opt<types::pointer>()}) {
                        curr = &ptr->underlying;
                    }
                    if (const auto ref{curr->get_data().as_opt<types::reference>()}) {
                        curr = &ref->underlying;
                    }
                    return *curr;
                };
                const auto& target{unwrap_ref(*outer_type)};
                const auto& member_name{home->ast.get_as<ast::identifier_expr>(dot->member).name};
                if (const auto dyn{target.get_data().as_opt<types::dyn_t>()}) {
                    return interface_member_discardable(dyn->interface, member_name, call_id)
                        .value_or(false);
                }
                const auto enclosing{target.get_data().visit(
                    [](const types::struct_t& s) -> stdx::option<const mod::module_t&> {
                        return s.enclosing;
                    },
                    [](const types::union_t& u) -> stdx::option<const mod::module_t&> {
                        return u.enclosing;
                    },
                    [](const types::enum_t& e) -> stdx::option<const mod::module_t&> {
                        return e.enclosing;
                    },
                    [](const auto&) -> stdx::option<const mod::module_t&> { return stdx::none; })};
                if (!enclosing || !target.has_symbol_table_idx()) { return false; }
                sym = ctx_.registry.get_from_opt(target.get_symbol_table_idx(), member_name);
                if (!sym) { return impl_method_discardable(target, member_name, call_id); }
                home = enclosing;
            }
        } else {
            return false;
        }
        if (!sym) { return false; }

        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return false; }
        const auto decl{home->ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl) { return false; }
        const auto fn_literal{decl->value ? home->ast.get_as_opt<ast::function_expr>(*decl->value)
                                          : stdx::none};
        // A literal's list and its declaration's both fold onto the literal
        const auto owner{fn_literal ? ast::node_id{*decl->value} : ast::node_id{*node}};
        if (const auto holds{discardable_holds(*home, owner, decl->attributes, call_id)}) {
            return *holds;
        }
        if (fn_literal) {
            return discardable_holds(*home, owner, fn_literal->attributes, call_id).value_or(false);
        }

        // Follow a direct `const g = f` / `const g = m.f` re-export to the real declaration.
        if (decl->value && (home->ast.get_as_opt<ast::identifier_expr>(*decl->value) ||
                            home->ast.get_as_opt<ast::dot_expr>(*decl->value))) {
            fn_node = *decl->value;
            continue;
        }
        return false;
    }
    return false;
}

auto type_resolver::check_unused_result(ast::node_id stmt_id, const ast::expr_stmt& stmt) -> void {
    const auto value_type{resolving_.get_sema_type_opt(stmt.expression)};
    if (!value_type || value_type->is_poison()) { return; }
    switch (value_type->get_kind()) {
    case type_kind::VOID_:
    case type_kind::NORETURN:
    case type_kind::AUTO:     return;
    default:                  break;
    }

    if (is_statement_form(resolving_.ast, stmt.expression)) { return; }

    const auto call{resolving_.ast.get_as_opt<ast::call_expr>(*stmt.expression)};
    if (call) {
        // `@builtin(...)` calls (`@expect`, `@memcpy`, …) are their own category, not covered.
        if (const auto fn_ty{resolving_.get_sema_type_opt(call->function)};
            fn_ty && fn_ty->get_data().is<types::builtin_function>()) {
            return;
        }
        if (callee_is_discardable(*stmt.expression, *call)) { return; }
        ctx_.diags.emplace_back(
            "result of this call is unused; bind it, pass it on, `_ =` it, or mark the callee "
            "'@[discardable]'",
            error::UNUSED_RESULT,
            resolving_.ast.location_of(stmt_id));
        return;
    }

    ctx_.diags.emplace_back("result of this expression is unused; bind it, or `_ =` it to "
                            "discard it on purpose",
                            error::UNUSED_RESULT,
                            resolving_.ast.location_of(stmt_id));
}

} // namespace ghoti::sema
