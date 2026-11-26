#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gsl/span>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <vector>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "gsl/pointers"
#include "gsl/util"
#include "internal.hh"
#include "stdx/assert.hh"
#include "stdx/utility.hh"

namespace ghoti::uir::sema {

namespace {

constexpr std::string_view NOT_A_FUNCTION{
    "'@export' expects a function declared with 'const', like '@export(add, .{ .name = \"add\" "
    "})'"};

} // namespace

auto compilation::add_export(const unit& u, function_export exported, const source& src)
    -> result<void> {
    const auto earlier{std::ranges::find(exports_, exported.name, &function_export::name)};
    if (earlier != exports_.end()) {
        // Analyzing the same `@export` again is no conflict
        if (earlier->fn.get() == exported.fn.get()) { return {}; }

        // The entry code's C `main` stands in for the program's `main` export
        if (exported.name != "main" || (!exported.from_start && !earlier->from_start)) {
            return report(u,
                          src,
                          gs::error::TYPE_MISMATCH,
                          fmt::format("The symbol '{}' is already exported at {}:{}",
                                      exported.name,
                                      earlier->site.line,
                                      earlier->site.column));
        }
    }

    request_body(*exported.fn);
    exports_.emplace_back(std::move(exported));
    return {};
}

auto compilation::apply_exports() -> void {
    for (auto fn : gir_.get_functions()) {
        bool first{true};
        for (const auto& exported : exports_) {
            if (exported.fn->symbol != fn->get_name()) { continue; }

            // The first names the function's own symbol, and each further one is an alias of it
            if (first) {
                fn->set_linkage(gir::linkage::EXPORT);
                fn->set_link_name(exported.name);
                fn->set_weak(exported.weak);
                auto attributes{fn->get_attributes()};
                attributes.visibility = exported.visibility;
                fn->set_attributes(attributes);
                first = false;
            } else {
                fn->add_alias({exported.name, exported.weak, exported.visibility});
            }
        }
    }
}

auto analyzer::export_target(ref arg, const source& src) -> result<stdx::option<instance&>> {
    const auto value{TRY(resolve_value(arg, src))};
    if (const auto fn{value.as_opt<function_ref>()}) {
        if (fn->fn->env) { return stdx::none; }
        return *fn->fn;
    }

    // A function passed to a `comptime` parameter
    const auto index{comptime_value(value)};
    const auto fn{index ? comp_.const_function(*index) : stdx::none};
    if (!fn || !fn->captures.empty() || fn->env) { return stdx::none; }
    return *fn;
}

auto analyzer::builtin_export(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 2) { return comp_.unsupported(u_, src, "an `@export` of this arity"); }

    const auto& target_src{attribute_arg_src(args[0], src)};
    if (const auto generic{resolve_value(args[0], target_src)};
        generic && generic->is<generic_ref>()) {
        return comp_.report(u_,
                            target_src,
                            gs::error::TYPE_MISMATCH,
                            "'@export' can't export a generic function; export a non-generic "
                            "wrapper that calls one instantiation");
    }

    const auto target{TRY(export_target(args[0], target_src))};
    if (!target) {
        return comp_.report(u_, target_src, gs::error::TYPE_MISMATCH, std::string{NOT_A_FUNCTION});
    }

    auto& fn{*target};
    if (fn.return_type->get_kind() == gs::type_kind::TYPE) {
        return comp_.report(u_,
                            target_src,
                            gs::error::TYPE_MISMATCH,
                            "'@export' can't export a function that returns a type as it only "
                            "exists at compile time");
    }

    const auto& owner{*fn.owner->uir};
    const auto& d{owner.decl_at(fn.decl)};
    const auto  fn_name{owner.string(d.name)};
    if (d.has(decl_flags::EXPORT)) {
        return comp_.report(u_,
                            target_src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'{}' is already exported by its declaration; give every "
                                        "name it needs with '@export' instead",
                                        fn_name));
    }

    // The options fold to a `builtin.ExportOptions` value
    const auto& options_src{attribute_arg_src(args[1], src)};
    auto&       options_type{*TRY(comp_.prelude_type("ExportOptions", u_, options_src))};
    const auto  options{[&] {
        const auto scope{comptime_scope()};
        return attribute_value(args[1], options_type, options_src);
    }()};
    auto&       pool{comp_.constants()};
    const auto  agg{options ? pool.value_at(*options).data.as_opt<ip::aggregate_value>()
                            : stdx::none};
    const auto  field{[&](std::string_view name) -> stdx::option<ip::index> {
        const auto at{field_index(options_type, name, options_src)};
        if (!agg || !at || *at >= agg->elements.size()) { return stdx::none; }
        return agg->elements[*at];
    }};

    const auto name_index{field("name")};
    const auto name{name_index ? known_text(*name_index) : stdx::none};
    if (!name) {
        return comp_.report(u_,
                            options_src,
                            gs::error::TYPE_MISMATCH,
                            "'@export' options must be a compile-time 'builtin.ExportOptions' "
                            "with a 'name'");
    }

    if (name->empty() || name->contains('\0')) {
        return comp_.report(u_,
                            options_src,
                            gs::error::TYPE_MISMATCH,
                            "An exported symbol name can't be empty or hold a zero byte");
    }

    const auto variant{[&](std::string_view name) -> std::string_view {
        const auto index{field(name)};
        return index ? variant_name(*index).value_or(std::string_view{}) : std::string_view{};
    }};
    const auto visibility{ast::symbol_visibility_from_name(variant("visibility"))
                              .value_or(ast::symbol_visibility::DEFAULT)};
    if (visibility == ast::symbol_visibility::PROTECTED &&
        !codegen::resolve_target_triple(comp_.ctx().target_opts.triple_str).isOSBinFormatELF()) {
        return comp_.report(u_,
                            options_src,
                            gs::error::TYPE_MISMATCH,
                            "Visibility '.protected' needs an ELF target");
    }

    // Exporting a function as `main` makes it the program's entry point
    const bool from_start{u_.source->is_start};
    if (*name == "main" && !from_start) {
        const bool named{fn.owner->index == u_.index && as_inst(args[0]) &&
                         mod().tag_of(*as_inst(args[0])) == tag::decl_val};
        if (!named) {
            return comp_.report(u_,
                                target_src,
                                gs::error::TYPE_MISMATCH,
                                "A function exported as 'main' must be declared in the module "
                                "that exports it, and named directly");
        }

        const auto& root{mod().root_namespace().decls};
        const bool  declares_main{std::ranges::any_of(
            root, [&](decl_id id) { return mod().string(mod().decl_at(id).name) == "main"; })};
        if (declares_main && fn_name != "main") {
            return comp_.report(u_,
                                target_src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("'{}' can't be exported as 'main' because this module "
                                            "already declares 'main'",
                                            fn_name));
        }

        comp_.ctx().user_main_name = std::string{fn_name};
    }

    TRY(comp_.add_export(u_,
                         {
                             .fn         = &fn,
                             .name       = std::move(*name),
                             .weak       = variant("linkage") == "weak",
                             .visibility = visibility,
                             .site       = builtin_name_location(src),
                             .from_start = from_start,
                         },
                         options_src));
    return void_value();
}

auto analyzer::known_text(ip::index value) -> stdx::option<std::string> {
    const auto& data{comp_.constants().value_at(value).data};
    if (const auto text{data.as_opt<std::string>()}) { return *text; }
    if (const auto agg{data.as_opt<ip::aggregate_value>()}) {
        return known_bytes(*agg, comp_.ctx().get_int(8, false));
    }

    return stdx::none;
}

auto compilation::test_units() const -> std::vector<bool> {
    // Tests are found through the imports written in another discovered module's tests
    std::vector<bool> found(units_.size(), false);
    const auto        root{std::ranges::find_if(
        units_, [&](const unit& u) { return u.source.get() == prog_.root.get(); })};
    if (root == units_.end()) { return found; }

    std::vector<gsl::not_null<const unit*>> frontier{&*root};
    found[root->index] = true;
    while (!frontier.empty()) {
        const auto& u{*frontier.back()};
        frontier.pop_back();
        const auto& ast{u.source->ast};
        for (const auto decl : u.uir->root_namespace().decls) {
            const auto& d{u.uir->decl_at(decl)};
            if (d.kind != decl_kind::TEST || !d.node) { continue; }
            // An import under a `@cfg` counts when AstGen lowered it
            const auto& test{ast.get_as<ast::test_stmt>(*d.node)};
            auto        stmts{ast.get_as<ast::block_stmt>(test.block).statements};
            while (!stmts.empty()) {
                const auto stmt{stmts.back()};
                stmts.pop_back();
                if (const auto cfg{ast.get_as_opt<ast::cfg_stmt>(ast::node_id{stmt})}) {
                    for (const auto& arm : cfg->arms) {
                        stmts.insert(stmts.end(), arm.items.begin(), arm.items.end());
                    }
                    continue;
                }

                const auto imports{u.uir->imports()};
                const auto import{std::ranges::find_if(imports, [&](const import_info& info) {
                    return info.node.get_index() == stmt.get_index() &&
                           info.node.get_kind() == stmt.get_kind();
                })};
                if (import == imports.end() || !import->path) { continue; }
                for (const auto& target : units_) {
                    if (target.source->path != *import->path || found[target.index]) { continue; }
                    found[target.index] = true;
                    frontier.emplace_back(&target);
                }
            }
        }
    }

    return found;
}

auto compilation::analyze_test(const unit& u, decl_id decl) -> void {
    const auto& m{*u.uir};
    const auto& d{m.decl_at(decl)};
    auto&       bool_type{ctx_.get_builtin_resolved_type(gs::type_kind::BOOL)};
    auto&       fn_type{ctx_.get_function({}, bool_type, false, ast::calling_convention::C, false)};
    const auto  body{m.body(d.value_body)};
    if (body.empty()) { return; }

    // Every test is a `fn(): bool` of its own
    auto& fn{*instances_.emplace_back(arena_.make<instance>(instance{
        .owner       = &units_[u.index],
        .decl        = decl,
        .func        = body.front(),
        .symbol      = fmt::format("__ghoti_test_fn_{}", tests_++),
        .type        = &fn_type,
        .params      = {},
        .return_type = &bool_type,
        .queued      = true,
        .analyzed    = true,
        .attributes  = gs::resolved_attributes{},
    }))};
    auto& gir_fn{gir_.add_function(fn.symbol, fn_type, true)};
    gir_fn.set_test_desc(d.name.len != 0 ? std::string{m.string(d.name)}
                                         : fmt::format("anonymous_test{}", anonymous_tests_++));
    const auto at{location_of(u, *d.node).one_based()};
    gir_fn.set_test_location(
        u.source->path.string(), static_cast<u32>(at.line), static_cast<u32>(at.column));

    analyzer a{*this, u, fn, gir_fn};
    DISCARD(a.analyze_test(body));
}

auto analyzer::analyze_test(gsl::span<const inst_id> body) -> result<void> {
    builder_.set_insert_point(*gir_fn_, gir_fn_->add_segment());
    gir_fn_->set_attributes(*fn_->attributes);
    const auto saved{comp_.limits()};
    const auto restore{gsl::finally([&] { comp_.limits() = saved; })};
    const auto out{analyze_body(body)};
    if (!out && out.error() == stop::UNSUPPORTED) { return stdx::err{stop::UNSUPPORTED}; }
    if (terminated()) { return {}; }

    // A test that reaches its end passes
    auto& bool_type{builtin(gs::type_kind::BOOL)};
    builder_.emit_return(constant(comp_.constants().intern(true, bool_type), bool_type));
    return {};
}

auto analyzer::builtin_src(const source& src) -> result<operand> {
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    auto&      location_type{*TRY(comp_.prelude_type("SourceLocation", u_, src))};
    auto&      u32_type{ctx.get_int(32, false)};
    auto&      path_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    const auto at{builtin_name_location(src)};
    const auto file{pool.intern(u_.source->path.string(), path_type)};
    const auto line{pool.intern(u64{at.line}, u32_type)};
    const auto column{pool.intern(u64{at.column}, u32_type)};
    if (is_comptime()) {
        return operand{pool.intern(ip::aggregate_value{{file, line, column}}, location_type)};
    }

    std::vector<gir::value> args;
    args.emplace_back(constant(file, path_type));
    args.emplace_back(constant(line, u32_type));
    args.emplace_back(constant(column, u32_type));
    const auto result{builder_.emit_builtin_call("@src", std::move(args), location_type)};
    return gir::value{*result, location_type};
}

auto analyzer::builtin_skip(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() > 1) { return comp_.unsupported(u_, src, "a `@skip` of this arity"); }
    auto&       ctx{comp_.ctx()};
    auto&       pool{comp_.constants()};
    auto&       text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    gir::value  message{constant(pool.intern(std::string{}, text_type), text_type)};
    std::string known_message;
    if (!args.empty()) {
        // A known message is passed as its text
        const auto given{TRY(resolve_value(args.front(), src))};
        const auto known{comptime_value(given)};
        if (const auto text{known ? known_text(*known) : stdx::none}) {
            known_message = *text;
            message       = constant(pool.intern(*text, text_type), text_type);
        } else if (is_comptime()) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                "@skip message must be a compile-time-constant string");
        } else {
            const auto coerced{TRY(coerce_to(given, text_type, src, {}))};
            message = TRY(as_runtime(coerced, src));
        }
    }

    // At compile time the whole evaluation is left, and the test skips where it began
    if (is_comptime()) { return stdx::err{comp_.skip(std::move(known_message))}; }
    return emit_skip(message, src);
}

auto analyzer::emit_skip(const gir::value& message, const source& src) -> result<operand> {
    auto& ctx{comp_.ctx()};
    auto& pool{comp_.constants()};
    auto& u32_type{ctx.get_int(32, false)};
    auto& text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    DISCARD(TRY(comp_.prelude_function("skip_handler", u_, src)));
    const auto              at{builtin_name_location(src)};
    std::vector<gir::value> call_args;
    call_args.emplace_back(message);
    call_args.emplace_back(constant(pool.intern(u_.source->path.string(), text_type), text_type));
    call_args.emplace_back(constant(pool.intern(u64{at.line}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(u64{at.column}, u32_type), u32_type));
    DISCARD(
        builder_.emit_builtin_call("@skip", std::move(call_args), builtin(gs::type_kind::VOID_)));

    // A skipped test passes, and a `@[testing]` helper never resumes, since the runner leaves it
    const bool helper{fn_ && fn_->attributes && fn_->attributes->testing};
    if (helper) {
        builder_.emit_unreachable();
    } else {
        auto& bool_type{builtin(gs::type_kind::BOOL)};
        builder_.emit_return(constant(pool.intern(true, bool_type), bool_type));
    }

    return comp_.constants().intern(ip::undefined_value{}, builtin(gs::type_kind::NORETURN));
}

auto analyzer::comptime_expect(gsl::span<const ref> args, const source& src, bool require)
    -> result<operand> {
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto cond{TRY(resolve_value(args.front(), src))};
    const auto known{comptime_value(TRY(coerce_to(cond, bool_type, src, {})))};
    const auto held{known ? comp_.constants().value_at(*known).data.as_opt<bool>() : stdx::none};
    if (!held) { return comp_.unsupported(u_, src, "an `@expect` of an unknown condition"); }
    if (*held) {
        return require ? void_value() : operand{comp_.constants().intern(true, bool_type)};
    }

    std::string message{require ? "Compile-time requirement failed"
                                : "Compile-time expectation failed"};
    if (args.size() > 1) {
        const auto given{TRY(resolve_value(args[1], src))};
        const auto text{comptime_value(given)};
        if (const auto known_message{text ? known_text(*text) : stdx::none}) {
            message = *known_message;
        }
    }

    const auto failed{comp_.report(u_, src, gs::error::STATIC_ASSERTION_FAILED, message)};
    if (require) { return failed; }
    return operand{comp_.constants().intern(false, bool_type)};
}

auto lists_discardable(const module_t& m, gsl::span<const inst_id> attrs) -> bool {
    return std::ranges::any_of(attrs, [&](inst_id inst) {
        if (m.tag_of(inst) != tag::attribute) { return false; }
        record_reader rec{m, m.data_of(inst).pl.extra};
        if (static_cast<ast::attribute_kind>(rec.u32_field()) != ast::attribute_kind::DISCARDABLE) {
            return false;
        }

        const auto args{rec.refs_field()};
        return args.empty() || as_well_known(args.front()) == well_known::bool_true;
    });
}

auto compilation::is_discardable(instance& fn) -> bool {
    // A generic's instance folds its attributes with its body, which may depend on its parameters
    if (!fn.attributes && !fn.analyzed && !fn.comptime_only && !fn.bindings.empty()) {
        DISCARD(analyze_body(fn));
    }

    if (fn.attributes && fn.attributes->discardable) { return true; }
    const auto& m{*fn.owner->uir};
    const auto& d{m.decl_at(fn.decl)};
    if (!fn.attributes) {
        if (d.attrs_body && lists_discardable(m, m.body(*d.attrs_body))) { return true; }
        if (std::to_underlying(fn.func) != 0 && m.tag_of(fn.func) == tag::func) {
            record_reader rec{m, m.data_of(fn.func).pl.extra};
            rec.skip_body().skip_body().skip_body().skip_u32().skip_u32();
            if (lists_discardable(m, rec.body_field())) { return true; }
        }
    }

    // An impl's method is discardable when its interface's member is
    if (!d.owner || m.tag_of(*d.owner) != tag::impl_decl) { return false; }
    const auto name{m.string(d.name)};
    for (const auto& [key, applied] : impl_instances_) {
        if (!applied || applied->entry->inst != *d.owner ||
            applied->entry->owner.get() != fn.owner.get() || !applied->interface) {
            continue;
        }

        const auto n{nominal_of(*applied->interface)};
        if (!n) { continue; }
        for (const auto& method : interface_methods(*n)) {
            if (method.name == name && lists_discardable(*n->owner->uir, method.attrs)) {
                return true;
            }
        }
    }

    return false;
}

auto analyzer::analyze_unused(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto& d{mod().data_of(inst)};
    const auto  value{resolve(d.un.operand, src)};
    if (!value) { return void_value(); }

    auto& type{type_of(*value)};
    switch (type.get_kind()) {
    case gs::type_kind::VOID_:
    case gs::type_kind::NORETURN:
    case gs::type_kind::AUTO:
    case gs::type_kind::UNDEFINED: return void_value();
    default:                       break;
    }
    if (type.is_poison()) { return void_value(); }

    if (d.un.aux == 0) {
        return comp_.report(u_,
                            src,
                            gs::error::UNUSED_RESULT,
                            "result of this expression is unused; bind it, or `_ =` it to "
                            "discard it on purpose");
    }

    const auto call{as_inst(d.un.operand)};
    const auto callee{call ? callees_.find(*call) : callees_.end()};
    if (callee != callees_.end() && comp_.is_discardable(*callee->second)) { return void_value(); }
    if (call && discardable_calls_.contains(*call)) { return void_value(); }
    return comp_.report(u_,
                        src,
                        gs::error::UNUSED_RESULT,
                        "result of this call is unused; bind it, pass it on, `_ =` it, or mark the "
                        "callee '@[discardable]'");
}

auto compilation::root_unit() const -> const unit& {
    const auto root{std::ranges::find_if(
        units_, [&](const unit& u) { return u.source.get() == prog_.root.get(); })};
    ASSERT(root != units_.end(), "the root module is lowered");
    return *root;
}

auto compilation::start_unit() const -> stdx::option<const unit&> {
    const auto start{
        std::ranges::find_if(units_, [](const unit& u) { return u.source->is_start; })};
    if (start == units_.end()) { return stdx::none; }
    return *start;
}

auto compilation::named_decl(const unit& u, std::string_view name, const source& use)
    -> stdx::option<result<operand>> {
    for (const auto decl : u.uir->root_namespace().decls) {
        if (u.uir->string(u.uir->decl_at(decl).name) == name) { return analyze_decl(u, decl, use); }
    }

    return stdx::none;
}

auto analyzer::require_start(std::string_view name, const source& src) -> result<void> {
    if (u_.source->is_start) { return {}; }
    return comp_.report(
        u_,
        src,
        gs::error::TYPE_MISMATCH,
        fmt::format("'{}()' is only valid in the program's entry code (`std/start.gh`)", name));
}

auto analyzer::builtin_entry_main(const source& src) -> result<operand> {
    TRY(require_start("@entryMain", src));

    // Entry code analyzed on its own has no `main` to call, so its value is unknown
    auto& ctx{comp_.ctx()};
    if (!comp_.start_unit() || comp_.root_unit().source->is_start) {
        auto& fn_type{ctx.get_function(
            {}, builtin(gs::type_kind::VOID_), false, ast::calling_convention::C, false)};
        return comp_.constants().intern(ip::undefined_value{}, fn_type);
    }

    if (auto main{comp_.named_decl(comp_.root_unit(), ctx.user_main_name, src)}) {
        return std::move(*main);
    }

    return comp_.report(
        u_,
        src,
        gs::error::TYPE_MISMATCH,
        fmt::format("'@entryMain()': the program has no '{}' function", ctx.user_main_name));
}

auto analyzer::builtin_entry_test_runner(const source& src) -> result<operand> {
    TRY(require_start("@entryTestRunner", src));

    // The program's own runner, then `std.testing`'s, then `builtin`'s
    constexpr std::string_view RUNNER{"test_runner"};
    if (auto own{comp_.named_decl(comp_.root_unit(), RUNNER, src)}) { return std::move(*own); }
    if (const auto std_mod{comp_.ctx().modules.find_loaded_library_module("std")}) {
        for (const auto& u : comp_.units()) {
            if (u.source.get() != &*std_mod) { continue; }
            const auto testing{comp_.named_decl(u, "testing", src)};
            const auto testing_mod{testing && *testing ? (*testing)->as_opt<module_ref>()
                                                       : stdx::none};
            if (!testing_mod) { break; }
            if (auto runner{comp_.named_decl(*testing_mod->target, RUNNER, src)}) {
                return std::move(*runner);
            }
        }
    }

    return comp_.prelude_decl(RUNNER, u_, src);
}

auto analyzer::builtin_test_functions(const source& src) -> result<operand> {
    TRY(require_start("@testFunctions", src));
    TRY(require_runtime(src));

    // Lowering builds the table once every test function is known
    auto&      ctx{comp_.ctx()};
    auto&      test_type{*TRY(comp_.prelude_type("Test", u_, src))};
    auto&      tests_type{ctx.get_slice(gs::types::mut::CONSTANT, false, test_type)};
    const auto result{builder_.emit_builtin_call("@testFunctions", {}, tests_type)};
    return gir::value{*result, tests_type};
}

} // namespace ghoti::uir::sema
