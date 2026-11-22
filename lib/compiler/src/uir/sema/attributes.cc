#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/codegen/target.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"

namespace ghoti::uir::sema {

namespace {

using ast::attribute_kind;
using ast::attribute_target;

[[nodiscard]] auto attribute_name(attribute_kind kind) -> std::string_view {
    return ast::attribute_spec_of(kind).name;
}

} // namespace

auto declares_literal(const module_t& m, const decl& d, inst_id func) -> bool {
    const auto value{m.body(d.value_body)};
    return !value.empty() && m.tag_of(value.back()) == tag::yield &&
           m.data_of(value.back()).un.operand == inst_ref(func);
}

auto analyzer::attribute_arg_src(ref arg, const source& fallback) const -> const source& {
    if (const auto inst{as_inst(arg)}) { return src_of(*inst); }
    return fallback;
}

auto analyzer::attribute_value(ref arg, stdx::option<gs::type&> expect, const source& src)
    -> stdx::option<ip::index> {
    auto value{resolve_value(arg, src)};
    if (value && expect) {
        value = coerce_to(*value, *expect, src, {.what = use_site::kind::STORE});
    }

    if (!value) { return stdx::none; }
    return comptime_value(*value);
}

auto analyzer::attribute_bool(const attribute_item& item) -> bool {
    if (item.args.empty()) { return true; }
    const auto& src{attribute_arg_src(item.args.front(), item.src)};
    const auto  index{attribute_value(item.args.front(), builtin(gs::type_kind::BOOL), src)};
    const auto  folded{index ? comp_.constants().value_at(*index).data.as_opt<bool>() : stdx::none};
    if (folded) { return *folded; }
    DISCARD(comp_.report(u_,
                         src,
                         gs::error::ILLEGAL_ATTRIBUTE,
                         fmt::format("Attribute '{}' requires a compile-time 'bool' argument",
                                     attribute_name(item.kind))));
    return false;
}

auto analyzer::attribute_variant(const attribute_item& item, ref arg, std::string_view enum_name)
    -> stdx::option<std::string> {
    const auto& src{attribute_arg_src(arg, item.src)};
    const auto  enum_type{comp_.prelude_type(enum_name, u_, src)};
    if (enum_type) {
        const auto index{attribute_value(arg, **enum_type, src)};
        if (const auto variant{index ? variant_name(*index) : stdx::none}) {
            return std::string{*variant};
        }
    }

    DISCARD(comp_.report(u_,
                         src,
                         gs::error::ILLEGAL_ATTRIBUTE,
                         fmt::format("Attribute '{}' requires a compile-time 'builtin.{}' value",
                                     attribute_name(item.kind),
                                     enum_name)));
    return stdx::none;
}

auto analyzer::attribute_alignment(const attribute_item& item) -> stdx::option<u64> {
    const auto& src{attribute_arg_src(item.args.front(), item.src)};
    const auto  index{attribute_value(item.args.front(), stdx::none, src)};
    const auto  exact{index ? int_of(comp_.constants().value_at(*index)) : stdx::none};
    const auto  small{exact ? exact->to_i128() : stdx::none};
    if (small && *small > 0 && (*small & (*small - 1)) == 0 &&
        *small <= std::numeric_limits<u32>::max()) {
        return static_cast<u64>(*small);
    }

    DISCARD(comp_.report(u_,
                         src,
                         gs::error::ILLEGAL_ATTRIBUTE,
                         "Attribute 'align' requires a compile-time power-of-two integer"));
    return stdx::none;
}

auto analyzer::attribute_visibility(const attribute_item& item)
    -> stdx::option<ast::symbol_visibility> {
    const auto variant{attribute_variant(item, item.args.front(), "Visibility")};
    const auto visibility{variant ? ast::symbol_visibility_from_name(*variant) : stdx::none};
    if (visibility != ast::symbol_visibility::PROTECTED) { return visibility; }

    const auto triple{codegen::resolve_target_triple(comp_.ctx().target_opts.triple_str)};
    if (triple.isOSBinFormatELF()) { return visibility; }
    DISCARD(comp_.report(u_,
                         attribute_arg_src(item.args.front(), item.src),
                         gs::error::ILLEGAL_ATTRIBUTE,
                         fmt::format("Visibility '.protected' needs an ELF target; '{}' has no "
                                     "protected symbols",
                                     triple.str())));
    return stdx::none;
}

auto analyzer::attribute_features(const attribute_item& item) -> std::vector<std::string> {
    const auto triple{codegen::resolve_target_triple(comp_.ctx().target_opts.triple_str)};
    const auto family{codegen::cpu_feature_family_of(triple)};

    std::vector<std::string> features;
    for (const auto arg : item.args) {
        const auto& src{attribute_arg_src(arg, item.src)};
        const auto  feature_type{comp_.prelude_type("CpuFeature", u_, src)};
        const auto  index{feature_type ? attribute_value(arg, **feature_type, src) : stdx::none};
        const auto  variant{index ? variant_name(*index) : stdx::none};
        const auto  at{variant && family ? std::ranges::find(family->ghoti_names, *variant)
                                         : std::vector<std::string>::const_iterator{}};
        if (!variant || !family || at == family->ghoti_names.end()) {
            DISCARD(comp_.report(u_,
                                 src,
                                 gs::error::ILLEGAL_ATTRIBUTE,
                                 "Attribute 'target_features' requires compile-time "
                                 "'builtin.CpuFeature' values"));
            continue;
        }

        const auto position{static_cast<usize>(at - family->ghoti_names.begin())};
        features.emplace_back(fmt::format("+{}", family->llvm_names[position]));
    }

    return features;
}

auto analyzer::attribute_items(gsl::span<const inst_id> body)
    -> result<std::vector<attribute_item>> {
    const auto                  scope{comptime_scope()};
    std::vector<attribute_item> items;
    for (const auto inst : body) {
        if (mod().tag_of(inst) != tag::attribute) {
            TRY(prepare(inst));
            continue;
        }

        auto       rec{record(inst)};
        const auto kind{static_cast<attribute_kind>(rec.u32_field())};
        const auto args{rec.refs_field()};
        items.emplace_back(attribute_item{
            .kind = kind, .args = {args.begin(), args.end()}, .src = src_of(inst), .inst = inst});
    }

    return items;
}

auto analyzer::check_deprecation_message(const attribute_item& item) -> void {
    if (item.args.empty()) { return; }
    const auto inst{as_inst(item.args.front())};
    if (inst && mod().tag_of(*inst) == tag::str) { return; }
    DISCARD(comp_.report(u_,
                         attribute_arg_src(item.args.front(), item.src),
                         gs::error::ILLEGAL_ATTRIBUTE,
                         "Attribute 'deprecated' takes a string literal message"));
}

auto analyzer::fold_attributes(gsl::span<const inst_id> body, const attribute_site& site)
    -> result<gs::resolved_attributes> {
    gs::resolved_attributes resolved;
    if (body.empty()) { return resolved; }
    const auto items{TRY(attribute_items(body))};

    // A declaration's function-only attributes reach its literal, so a kind may come twice
    std::vector<attribute_item> applied;
    for (const auto& item : items) {
        const auto seen{std::ranges::any_of(
            applied, [&](const attribute_item& earlier) { return earlier.kind == item.kind; })};
        if (seen) {
            DISCARD(comp_.report(u_,
                                 item.src,
                                 gs::error::ILLEGAL_ATTRIBUTE,
                                 fmt::format("Attribute '{}' is applied to both the declaration "
                                             "and its function literal",
                                             attribute_name(item.kind))));
            continue;
        }

        applied.emplace_back(item);
    }

    check_visibility_linkage(applied, site);
    for (const auto& item : applied) {
        const auto& spec{ast::attribute_spec_of(item.kind)};
        if (!static_cast<bool>(spec.targets & site.target)) {
            report_misplaced(item, site);
            continue;
        }

        switch (item.kind) {
        case attribute_kind::DISCARDABLE:
            if (site.returns_void) {
                DISCARD(comp_.report(
                    u_,
                    item.src,
                    gs::error::ILLEGAL_ATTRIBUTE,
                    "Attribute 'discardable' has no effect on a function that returns 'void'"));
            }
            resolved.discardable = attribute_bool(item);
            break;
        case attribute_kind::INLINE:
            if (const auto variant{attribute_variant(item, item.args.front(), "Inline")}) {
                const auto mode{ast::inline_mode_from_name(*variant)};
                if (mode != ast::inline_mode::DEFAULT) { resolved.inlining = mode; }
            }
            break;
        case attribute_kind::NAKED:       resolved.naked = attribute_bool(item); break;
        case attribute_kind::ALIGN:       resolved.alignment = attribute_alignment(item); break;
        case attribute_kind::DEPRECATED:  check_deprecation_message(item); break;
        case attribute_kind::VISIBILITY:  resolved.visibility = attribute_visibility(item); break;
        case attribute_kind::TESTING:     resolved.testing = true; break;
        case attribute_kind::NO_BUILTINS: resolved.no_builtins = attribute_bool(item); break;
        case attribute_kind::TARGET_FEATURES:
            resolved.target_features = attribute_features(item);
            break;
        }
    }

    if (resolved.naked && resolved.inlining == ast::inline_mode::ALWAYS) {
        const auto inline_item{
            std::ranges::find(applied, attribute_kind::INLINE, &attribute_item::kind)};
        DISCARD(comp_.report(u_,
                             inline_item->src,
                             gs::error::ILLEGAL_ATTRIBUTE,
                             "A 'naked' function cannot be 'inline(.always)'"));
    }

    if (resolved.naked && !resolved.inlining) { resolved.inlining = ast::inline_mode::NEVER; }
    return resolved;
}

auto analyzer::decl_attributes(const decl& d, const operand& value)
    -> result<gs::resolved_attributes> {
    const auto body{mod().body(d.attrs_body)};
    if (body.empty()) { return gs::resolved_attributes{}; }

    // A declaration that names a function is checked as one, its literal's own list aside
    const auto fn{value.as_opt<function_ref>()};
    const bool callable{fn || value.is<generic_ref>()};
    const auto resolved{TRY(fold_attributes(
        body,
        {.target       = callable ? attribute_target::FN_DECL : attribute_target::DECL,
         .returns_void = fn && fn->fn->return_type->get_kind() == gs::type_kind::VOID_,
         .declared_by  = d}))};

    // Without storage there is nothing to align
    const auto align{std::ranges::find_if(body, [&](inst_id inst) {
        return mod().tag_of(inst) == tag::attribute &&
               static_cast<attribute_kind>(record(inst).u32_field()) == attribute_kind::ALIGN;
    })};
    if (align != body.end() && !callable && d.has(decl_flags::COMPTIME)) {
        DISCARD(comp_.report(u_,
                             src_of(*align),
                             gs::error::ILLEGAL_ATTRIBUTE,
                             "Attribute 'align' needs storage; a 'const' declaration has none"));
    }

    return resolved;
}

auto analyzer::check_visibility_linkage(gsl::span<const attribute_item> items,
                                        const attribute_site&           site) -> void {
    const auto item{std::ranges::find(items, attribute_kind::VISIBILITY, &attribute_item::kind)};
    if (item == items.end()) { return; }
    const auto linked{decl_flags::PUBLIC | decl_flags::EXPORT | decl_flags::EXTERN |
                      decl_flags::WEAK};
    if (site.declared_by && (site.declared_by->flags & linked) != decl_flags::NONE) { return; }
    DISCARD(comp_.report(u_,
                         item->src,
                         gs::error::ILLEGAL_ATTRIBUTE,
                         "Attribute 'visibility' needs a symbol other objects can link to: a "
                         "'pub', 'export', 'extern', or 'weak' declaration"));
}

auto analyzer::report_misplaced(const attribute_item& item, const attribute_site& site) -> void {
    const auto name{attribute_name(item.kind)};
    if (site.target == attribute_target::FIELD) {
        DISCARD(comp_.report(u_,
                             item.src,
                             gs::error::ILLEGAL_ATTRIBUTE,
                             fmt::format("Attribute '{}' cannot be applied to a field", name)));
        return;
    }

    const auto* what{ast::routes_to_fn_literal(item.kind)
                         ? "a declaration whose initializer is not a function literal"
                         : "a non-function declaration"};
    DISCARD(comp_.report(u_,
                         item.src,
                         gs::error::ILLEGAL_ATTRIBUTE,
                         fmt::format("Attribute '{}' cannot be applied to {}", name, what)));
}

} // namespace ghoti::uir::sema
