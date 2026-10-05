#include "compiler/ast/attributes.hh"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::ast {

namespace {

constexpr auto CALLCONV_NAMES_TO_VALUES{string_utils::make_constexpr_map<calling_convention>(
    std::pair{"c", calling_convention::C},
    std::pair{"sysv", calling_convention::SYSV},
    std::pair{"win64", calling_convention::WIN64},
    std::pair{"stdcall", calling_convention::X86_STDCALL},
    std::pair{"fastcall", calling_convention::X86_FASTCALL},
    std::pair{"aapcs", calling_convention::AAPCS})};

constexpr auto CALLCONV_VALS_TO_NAMES{[] {
    stdx::fixed::enum_map<calling_convention, std::string_view> map{};
    for (const auto& [sv, conv] : CALLCONV_NAMES_TO_VALUES) { map[conv] = sv; }
    return map;
}()};

constexpr std::array ALL_ATTRIBUTES{
    attribute_spec{
        .name      = "discardable",
        .kind      = attribute_kind::DISCARDABLE,
        .min_args  = 0,
        .max_args  = 1,
        .targets   = attribute_target::FN_DECL | attribute_target::FN,
        .signature = "discardable(bool = true)",
        .doc       = "The function's result may be ignored without `_ = ...`",
    },
    attribute_spec{
        .name      = "inline",
        .kind      = attribute_kind::INLINE,
        .min_args  = 1,
        .max_args  = 1,
        .targets   = attribute_target::FN,
        .signature = "inline(builtin.Inline)",
        .doc = "How calls to the function are inlined: `.always`, `.never`, `.hint`, or `.default`",
    },
    attribute_spec{
        .name      = "naked",
        .kind      = attribute_kind::NAKED,
        .min_args  = 0,
        .max_args  = 1,
        .targets   = attribute_target::FN,
        .signature = "naked(bool = true)",
        .doc       = "Emits no prologue or epilogue; the body must be inline assembly",
    },
    attribute_spec{
        .name      = "align",
        .kind      = attribute_kind::ALIGN,
        .min_args  = 1,
        .max_args  = 1,
        .targets   = attribute_target::DECL | attribute_target::FN | attribute_target::FIELD,
        .signature = "align(power-of-two integer)",
        .doc = "Minimum alignment in bytes of the declaration's storage or the function's code",
    },
    attribute_spec{
        .name      = "deprecated",
        .kind      = attribute_kind::DEPRECATED,
        .min_args  = 0,
        .max_args  = 1,
        .targets   = attribute_target::DECL | attribute_target::FN_DECL | attribute_target::FIELD,
        .signature = "deprecated([]u8 = none)",
        .doc =
            "Uses warn, or fail with `--deprecated=error` with an optional message explaining why",
    },
    attribute_spec{
        .name      = "visibility",
        .kind      = attribute_kind::VISIBILITY,
        .min_args  = 1,
        .max_args  = 1,
        .targets   = attribute_target::DECL | attribute_target::FN_DECL | attribute_target::FN,
        .signature = "visibility(builtin.Visibility)",
        .doc = "Who outside the linked image sees the symbol: `.default`, `.hidden` (this image "
               "only), or `.protected` (ELF: exported but never preempted)",
    },
    attribute_spec{
        .name      = "testing",
        .kind      = attribute_kind::TESTING,
        .min_args  = 0,
        .max_args  = 0,
        .targets   = attribute_target::FN_DECL | attribute_target::FN,
        .signature = "testing",
        .doc = "A test helper: may use `@expect`, `@require`, and `@skip`, and is called only "
               "from tests and other `@[testing]` functions",
    },
};

[[nodiscard]] auto parse_attribute_args(syntax::parser& parser)
    -> stdx::result<std::vector<expr_handle>, syntax::diagnostic> {
    std::vector<expr_handle> args;
    if (!parser.peek_token_is(syntax::token_type_t::LPAREN)) { return args; }
    parser.advance(); // current == (
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        parser.advance();
        args.emplace_back(TRY(parser.parse_expression()));
        if (!parser.peek_token_is(syntax::token_type_t::COMMA)) { break; }
        parser.advance();
    }
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    return args;
}

[[nodiscard]] auto
check_arity(const attribute_spec& spec, usize arg_count, const syntax::token_t& name_token)
    -> stdx::result<void, syntax::diagnostic> {
    if (arg_count >= spec.min_args && arg_count <= spec.max_args) { return {}; }
    const auto expected{spec.min_args == spec.max_args
                            ? fmt::format("{}", spec.min_args)
                            : fmt::format("{} to {}", spec.min_args, spec.max_args)};
    return make_syntax_err(fmt::format("Attribute '{}' takes {} argument{}; found {}",
                                       spec.name,
                                       expected,
                                       spec.max_args == 1 ? "" : "s",
                                       arg_count),
                           syntax::error::ATTRIBUTE_ARITY,
                           name_token);
}

[[nodiscard]] auto parse_one_attribute(syntax::parser& parser, const attribute_list& seen)
    -> stdx::result<attribute, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::IDENT)) {
        return make_syntax_err(
            fmt::format("Expected an attribute name, found {}",
                        syntax::token_type::describe(parser.get_peek_token().type)),
            syntax::error::UNKNOWN_ATTRIBUTE,
            parser.get_peek_token());
    }
    parser.advance();
    const auto name_token{parser.get_current_token()};
    const auto spec{attribute_spec_of(name_token.slice)};
    if (!spec) {
        return make_syntax_err(fmt::format("Unknown attribute '{}'", name_token.slice),
                               syntax::error::UNKNOWN_ATTRIBUTE,
                               name_token);
    }
    if (seen.find(spec->kind)) {
        return make_syntax_err(fmt::format("Attribute '{}' may only appear once", spec->name),
                               syntax::error::DUPLICATE_ATTRIBUTE,
                               name_token);
    }

    const identifier_handle name{TRY(identifier_expr::parse(parser))};
    auto                    args{TRY(parse_attribute_args(parser))};
    TRY(check_arity(*spec, args.size(), name_token));
    return attribute{.name = name, .kind = spec->kind, .args = std::move(args)};
}

} // namespace

auto attribute_spec_of(std::string_view name) noexcept -> stdx::option<const attribute_spec&> {
    const auto it{std::ranges::find(ALL_ATTRIBUTES, name, &attribute_spec::name)};
    if (it == ALL_ATTRIBUTES.end()) { return stdx::none; }
    return *it;
}

auto all_attribute_specs() noexcept -> gsl::span<const attribute_spec> { return ALL_ATTRIBUTES; }

auto attribute_enum_variants(attribute_kind kind) noexcept -> gsl::span<const std::string_view> {
    using namespace std::string_view_literals;
    static constexpr std::array inline_variants{"always"sv, "never"sv, "hint"sv, "default"sv};
    static constexpr std::array visibility_variants{"default"sv, "hidden"sv, "protected"sv};

    switch (kind) {
    case attribute_kind::INLINE:     return inline_variants;
    case attribute_kind::VISIBILITY: return visibility_variants;
    default:                         return {};
    }
}

auto attribute_spec_of(attribute_kind kind) noexcept -> const attribute_spec& {
    return *std::ranges::find(ALL_ATTRIBUTES, kind, &attribute_spec::kind);
}

auto routes_to_fn_literal(attribute_kind kind) noexcept -> bool {
    return static_cast<bool>(attribute_spec_of(kind).targets & attribute_target::FN);
}

auto attribute_arg(const stdx::option<attribute_list>& attributes, attribute_kind kind) noexcept
    -> stdx::option<expr_handle> {
    if (!attributes) { return stdx::none; }
    const auto item{attributes->find(kind)};
    if (!item || item->args.empty()) { return stdx::none; }
    return item->args.front();
}

auto branch_hint_from_name(std::string_view name) noexcept -> stdx::option<branch_hint> {
    if (name == "none") { return branch_hint::NONE; }
    if (name == "likely") { return branch_hint::LIKELY; }
    if (name == "unlikely") { return branch_hint::UNLIKELY; }
    if (name == "cold") { return branch_hint::COLD; }
    if (name == "unpredictable") { return branch_hint::UNPREDICTABLE; }
    return stdx::none;
}

auto inline_mode_from_name(std::string_view name) noexcept -> stdx::option<inline_mode> {
    if (name == "always") { return inline_mode::ALWAYS; }
    if (name == "never") { return inline_mode::NEVER; }
    if (name == "hint") { return inline_mode::HINT; }
    if (name == "default") { return inline_mode::DEFAULT; }
    return stdx::none;
}

auto inline_mode_name(inline_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case inline_mode::ALWAYS:  return "always";
    case inline_mode::NEVER:   return "never";
    case inline_mode::HINT:    return "hint";
    case inline_mode::DEFAULT: return "default";
    }
    return "default";
}

auto symbol_visibility_name(symbol_visibility visibility) noexcept -> std::string_view {
    switch (visibility) {
    case symbol_visibility::DEFAULT:   return "default";
    case symbol_visibility::HIDDEN:    return "hidden";
    case symbol_visibility::PROTECTED: return "protected";
    }
    return "default";
}

auto symbol_visibility_from_name(std::string_view name) noexcept
    -> stdx::option<symbol_visibility> {
    if (name == "default") { return symbol_visibility::DEFAULT; }
    if (name == "hidden") { return symbol_visibility::HIDDEN; }
    if (name == "protected") { return symbol_visibility::PROTECTED; }
    return stdx::none;
}

auto parse_attribute_list(syntax::parser& parser)
    -> stdx::result<attribute_list, syntax::diagnostic> {
    const auto     open_token{parser.get_current_token()};
    attribute_list list;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACKET)) {
        list.items.emplace_back(TRY(parse_one_attribute(parser, list)));
        if (!parser.peek_token_is(syntax::token_type_t::COMMA)) { break; }
        parser.advance();
        list.force_break = parser.peek_token_is(syntax::token_type_t::RBRACKET);
    }
    TRY(parser.expect_peek(syntax::token_type_t::RBRACKET));

    if (list.items.empty()) {
        return make_syntax_err(
            "An attribute list may not be empty", syntax::error::MISPLACED_ATTRIBUTES, open_token);
    }
    if (parser.peek_token_is(syntax::token_type_t::AT_LBRACKET)) {
        return make_syntax_err("Only one attribute list may be applied; merge them into one",
                               syntax::error::DUPLICATE_ATTRIBUTE,
                               parser.get_peek_token());
    }
    return list;
}

auto calling_convention_from_name(std::string_view name) noexcept
    -> stdx::option<calling_convention> {
    return CALLCONV_NAMES_TO_VALUES.get_opt(name).materialize();
}

auto calling_convention_name(calling_convention conv) noexcept -> std::string_view {
    return CALLCONV_VALS_TO_NAMES[conv];
}

} // namespace ghoti::ast
