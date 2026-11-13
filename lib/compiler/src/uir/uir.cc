#include "compiler/uir/uir.hh"

#include <array>
#include <string_view>
#include <utility>

#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/id.hh"
#include "compiler/syntax/builtins.hh"

namespace ghoti::uir {

namespace {

using enum field_kind;

[[nodiscard]] auto describe_conv(u32 value) -> std::string_view {
    return magic_enum::enum_name(static_cast<ast::calling_convention>(value));
}

[[nodiscard]] auto describe_modifier(u32 value) -> std::string_view {
    return magic_enum::enum_name(static_cast<ast::type_modifier::modifier>(value));
}

[[nodiscard]] auto describe_builtin(u32 value) -> std::string_view {
    return syntax::builtin_name(static_cast<syntax::builtin_t>(value));
}

[[nodiscard]] auto describe_union_tag(u32 value) -> std::string_view {
    return magic_enum::enum_name(static_cast<union_tag>(value));
}

[[nodiscard]] auto describe_attribute(u32 value) -> std::string_view {
    return ast::attribute_spec_of(static_cast<ast::attribute_kind>(value)).name;
}

[[nodiscard]] auto describe_unwrap(u32 value) -> std::string_view {
    return magic_enum::enum_name(static_cast<unwrap_kind>(value));
}

// Bit names, indexed by bit position
constexpr std::array<std::string_view, 4> INT_FLAGS{"signed", "size", "char", "big"};
constexpr std::array<std::string_view, 6> PTR_FLAGS{
    "mut", "volatile", "poly", "sentinel", "place", "len_absent"};
constexpr std::array<std::string_view, 8> FN_FLAGS{
    "variadic", "extern", "dyn_fn", "impl_fn", "explicit_conv", "move", "type_expr", "testing"};
constexpr std::array<std::string_view, 2> AGG_FLAGS{"extern", "packed"};
constexpr std::array<std::string_view, 1> ENUM_FLAGS{"non_exhaustive"};
constexpr std::array<std::string_view, 6> PARAM_FLAGS{
    "comptime", "comptime_written", "pack", "self", "discarded", "written"};
constexpr std::array<std::string_view, 5> RANGE_FLAGS{"inclusive", "", "", "", "place"};
constexpr std::array<std::string_view, 4> LOOP_FLAGS{"unroll", "do_while", "unwraps", "has_cond"};
constexpr std::array<std::string_view, 2> MATCH_FLAGS{"comptime", "binding"};
constexpr std::array<std::string_view, 1> PUBLIC_FLAGS{"pub"};
constexpr std::array<std::string_view, 1> SIGNED_FLAGS{"signed"};
constexpr std::array<std::string_view, 5> ASM_OPTIONS{
    "volatile", "noreturn", "intel", "att", "align_stack"};

constexpr std::array CAPTURE_ITEMS{field{REF, "value"}, field{U32, "by_ref"}, field{STR, "name"}};
constexpr std::array DECL_ITEMS{field{DECL, "decl"}};
constexpr std::array NAME_ITEMS{field{STR, "name"}};
constexpr std::array BINDING_ITEMS{field{STR, "name"}, field{REF, "type"}};
constexpr std::array STRUCT_FIELD_ITEMS{field{STR, "name"},
                                        field{U32, "flags", {}, PUBLIC_FLAGS},
                                        field{BODY, "type"},
                                        field{BODY, "default"},
                                        field{BODY, "attrs"}};
constexpr std::array UNION_FIELD_ITEMS{
    field{STR, "name"}, field{BODY, "type"}, field{BODY, "attrs"}};
constexpr std::array ENUM_VALUE_ITEMS{field{STR, "name"}, field{BODY, "value"}};
constexpr std::array ASSOC_ITEMS{field{STR, "name"}, field{BODY, "type"}, field{BODY, "default"}};
constexpr std::array METHOD_ITEMS{field{STR, "name"},
                                  field{U32, "flags", {}, PUBLIC_FLAGS},
                                  field{BODY, "signature"},
                                  field{BODY, "attrs"}};
constexpr std::array INIT_ITEMS{field{STR, "name"}, field{REF, "value"}};
constexpr std::array ASM_OPERAND_ITEMS{field{STR, "constraint"}, field{REF, "value"}};
constexpr std::array ARM_ITEMS{field{REFS, "patterns"},
                               field{BODY, "pattern_body"},
                               field{REF, "capture"},
                               field{BODY, "guard"},
                               field{BODY, "body"}};
constexpr std::array PAT_INIT_ITEMS{field{STR, "name"}, field{REF, "pattern"}};
constexpr std::array IMPL_BOUND_ITEMS{field{U32, "param"}, field{REFS, "interfaces"}};

constexpr std::array INT_LIT{
    field{WORDS, "limbs"}, field{U32, "width"}, field{U32, "flags", {}, INT_FLAGS}};
constexpr std::array FLOAT_LIT{field{WORDS, "bits"}, field{U32, "width"}};
constexpr std::array BOOL_OP{field{REF, "lhs"}, field{BODY, "rhs"}};
constexpr std::array INT_TYPE{field{U32, "bits"}, field{U32, "flags", {}, SIGNED_FLAGS}};
constexpr std::array ELEM_TYPE{field{REF, "elem"}, field{U32, "flags", {}, PTR_FLAGS}};
constexpr std::array ARRAY_TYPE{
    field{REF, "len"}, field{REF, "elem"}, field{U32, "flags", {}, PTR_FLAGS}};
constexpr std::array FN_TYPE{field{REFS, "params"},
                             field{LIST, "names", NAME_ITEMS},
                             field{REF, "ret"},
                             field{U32, "conv", {}, {}, describe_conv},
                             field{U32, "flags", {}, FN_FLAGS}};
constexpr std::array DYN_TYPE{field{REF, "interface"}, field{LIST, "bindings", BINDING_ITEMS}};
constexpr std::array STRUCT_DECL{field{U32, "flags", {}, AGG_FLAGS},
                                 field{LIST, "captures", CAPTURE_ITEMS},
                                 field{LIST, "fields", STRUCT_FIELD_ITEMS},
                                 field{LIST, "decls", DECL_ITEMS}};
constexpr std::array UNION_DECL{field{U32, "flags", {}, AGG_FLAGS},
                                field{U32, "tag", {}, {}, describe_union_tag},
                                field{BODY, "tag_type"},
                                field{LIST, "captures", CAPTURE_ITEMS},
                                field{LIST, "fields", UNION_FIELD_ITEMS},
                                field{LIST, "decls", DECL_ITEMS}};
constexpr std::array ENUM_DECL{field{U32, "flags", {}, ENUM_FLAGS},
                               field{BODY, "underlying"},
                               field{LIST, "captures", CAPTURE_ITEMS},
                               field{LIST, "values", ENUM_VALUE_ITEMS},
                               field{LIST, "decls", DECL_ITEMS}};
constexpr std::array INTERFACE_DECL{field{LIST, "captures", CAPTURE_ITEMS},
                                    field{BODY, "header"},
                                    field{LIST, "assoc_types", ASSOC_ITEMS},
                                    field{LIST, "assoc_consts", ASSOC_ITEMS},
                                    field{LIST, "methods", METHOD_ITEMS}};
constexpr std::array FIELD_ACCESS{field{REF, "object"}, field{STR, "name"}};
constexpr std::array FIELD_PTR{
    field{REF, "object"}, field{STR, "name"}, field{U32, "flags", {}, PTR_FLAGS}};
constexpr std::array ELEM_PTR{
    field{REF, "object"}, field{REF, "index"}, field{U32, "flags", {}, PTR_FLAGS}};
constexpr std::array SLICE{
    field{REF, "object"}, field{REF, "lo"}, field{REF, "hi"}, field{U32, "flags", {}, RANGE_FLAGS}};
constexpr std::array PARAM{field{STR, "name"},
                           field{BODY, "type"},
                           field{BODY, "default"},
                           field{U32, "flags", {}, PARAM_FLAGS},
                           field{U32, "modifier", {}, {}, describe_modifier}};
constexpr std::array BIND{
    field{STR, "name"}, field{U32, "slot"}, field{U32, "modifier", {}, {}, describe_modifier}};
constexpr std::array ALLOC{field{STR, "name"}, field{REF, "type"}};
constexpr std::array LET_VALUE{field{STR, "name"}, field{REF, "type"}, field{REF, "init"}};
constexpr std::array CONST_LOCAL{field{STR, "name"}, field{REF, "type"}, field{BODY, "value"}};
constexpr std::array REF_OF{field{REF, "operand"}, field{U32, "flags", {}, PTR_FLAGS}};
constexpr std::array INIT_STRUCT{field{REF, "type"}, field{LIST, "fields", INIT_ITEMS}};
constexpr std::array INIT_ARRAY{field{REF, "type"}, field{REFS, "values"}};
constexpr std::array ARRAY_LIT{field{REF, "len"},
                               field{REF, "elem"},
                               field{U32, "flags", {}, PTR_FLAGS},
                               field{REFS, "items"}};
constexpr std::array RANGE{
    field{REF, "lo"}, field{REF, "hi"}, field{U32, "flags", {}, RANGE_FLAGS}};
constexpr std::array CALL{
    field{REF, "callee"}, field{REF, "modifier"}, field{REFS, "args"}, field{WORDS, "pack_mask"}};
constexpr std::array FIELD_CALL{field{REF, "object"},
                                field{STR, "name"},
                                field{U32, "flags", {}, RANGE_FLAGS},
                                field{REF, "modifier"},
                                field{REFS, "args"},
                                field{WORDS, "pack_mask"}};
constexpr std::array BUILTIN{field{U32, "builtin", {}, {}, describe_builtin}, field{REFS, "args"}};
constexpr std::array ASM{field{STR, "template"},
                         field{REF, "result_type"},
                         field{LIST, "outputs", ASM_OPERAND_ITEMS},
                         field{LIST, "inputs", ASM_OPERAND_ITEMS},
                         field{LIST, "clobbers", NAME_ITEMS},
                         field{U32, "options", {}, ASM_OPTIONS}};
constexpr std::array BLOCK{field{STR, "label"}, field{BODY, "body"}};
constexpr std::array BLOCK_COMPTIME{field{STR, "label"}, field{BODY, "body"}};
constexpr std::array LOOP{field{STR, "label"},
                          field{U32, "flags", {}, LOOP_FLAGS},
                          field{BODY, "cond"},
                          field{BODY, "body"},
                          field{BODY, "cont"},
                          field{BODY, "else"}};
constexpr std::array FOR_ITER{field{STR, "label"},
                              field{U32, "flags", {}, LOOP_FLAGS},
                              field{REFS, "iterables"},
                              field{BODY, "body"},
                              field{BODY, "else"}};
constexpr std::array CONDBR{field{REF, "cond"}, field{BODY, "then"}, field{BODY, "else"}};
constexpr std::array EVAL_BRANCH{field{BODY, "comptime"}, field{BODY, "runtime"}};
constexpr std::array UNWRAP_BRANCH{field{REF, "operand"}, field{BODY, "then"}, field{BODY, "else"}};
constexpr std::array MATCH{field{REF, "operand"},
                           field{U32, "flags", {}, MATCH_FLAGS},
                           field{U32, "catch_all"},
                           field{LIST, "arms", ARM_ITEMS}};
constexpr std::array PAT_CAPTURE{field{STR, "name"},
                                 field{U32, "slot"},
                                 field{U32, "modifier", {}, {}, describe_modifier},
                                 field{REF, "pattern"}};
constexpr std::array PAT_VALUE{field{BODY, "value"}};
constexpr std::array PAT_RANGE{
    field{BODY, "lo"}, field{BODY, "hi"}, field{U32, "flags", {}, RANGE_FLAGS}};
constexpr std::array PAT_INIT{field{BODY, "type"}, field{LIST, "entries", PAT_INIT_ITEMS}};
constexpr std::array PAT_CALL{field{BODY, "callee"}, field{REFS, "args"}};
constexpr std::array UNWRAP{
    field{REF, "operand"}, field{U32, "kind", {}, {}, describe_unwrap}, field{BODY, "failure"}};
constexpr std::array FUNC{field{BODY, "params"},
                          field{BODY, "ret"},
                          field{BODY, "body"},
                          field{U32, "conv", {}, {}, describe_conv},
                          field{U32, "flags", {}, FN_FLAGS},
                          field{BODY, "attrs"},
                          field{LIST, "impl_bounds", IMPL_BOUND_ITEMS}};
constexpr std::array CLOSURE{field{REF, "func"}, field{LIST, "captures", CAPTURE_ITEMS}};
constexpr std::array ATTRIBUTE{field{U32, "kind", {}, {}, describe_attribute}, field{REFS, "args"}};
constexpr std::array IMPL_DECL{field{BODY, "params"},
                               field{BODY, "interface"},
                               field{BODY, "target"},
                               field{LIST, "decls", DECL_ITEMS}};

[[nodiscard]] auto skip_record(const module_t& mod, gsl::span<const field> schema, u32 at) -> u32 {
    const auto extra{mod.extra()};
    for (const auto& f : schema) {
        switch (f.kind) {
        case REF:
        case U32:
        case DECL:  at += 1; break;
        case STR:   at += 2; break;
        case BODY:
        case REFS:
        case WORDS: at += 1 + extra[at]; break;
        case LIST:  {
            const auto n{extra[at++]};
            for (u32 i{0}; i < n; ++i) { at = skip_record(mod, f.items, at); }
            break;
        }
        }
    }
    return at;
}

} // namespace

auto payload_of(tag t) noexcept -> payload_kind {
    switch (t) {
#define UIR_TAG(name, payload) \
    case tag::name: return payload_kind::payload;
#include "compiler/uir/tags.def"
#undef UIR_TAG
    }
    return payload_kind::NONE;
}

auto tag_name(tag t) noexcept -> std::string_view {
    switch (t) {
#define UIR_TAG(name, payload) \
    case tag::name: return #name;
#include "compiler/uir/tags.def"
#undef UIR_TAG
    }
    return "?";
}

auto well_known_name(well_known w) noexcept -> std::string_view {
    switch (w) {
#define UIR_WELL_KNOWN_NAME(name) \
    case well_known::name: return #name;
        FOREACH_UIR_WELL_KNOWN(UIR_WELL_KNOWN_NAME)
#undef UIR_WELL_KNOWN_NAME
    }
    return "?";
}

auto schema_of(tag t) noexcept -> gsl::span<const field> {
    switch (t) {
    case tag::int_lit:         return INT_LIT;
    case tag::float_lit:       return FLOAT_LIT;
    case tag::bool_and:
    case tag::bool_or:         return BOOL_OP;
    case tag::int_type:        return INT_TYPE;
    case tag::ptr_type:
    case tag::ref_type:
    case tag::slice_type:      return ELEM_TYPE;
    case tag::array_type:      return ARRAY_TYPE;
    case tag::fn_type:         return FN_TYPE;
    case tag::dyn_type:        return DYN_TYPE;
    case tag::struct_decl:     return STRUCT_DECL;
    case tag::union_decl:      return UNION_DECL;
    case tag::enum_decl:       return ENUM_DECL;
    case tag::interface_decl:  return INTERFACE_DECL;
    case tag::field_val:       return FIELD_ACCESS;
    case tag::field_ptr:       return FIELD_PTR;
    case tag::elem_ptr:        return ELEM_PTR;
    case tag::slice:           return SLICE;
    case tag::param:           return PARAM;
    case tag::bind:            return BIND;
    case tag::alloc_mut:
    case tag::alloc_comptime:  return ALLOC;
    case tag::let_value:       return LET_VALUE;
    case tag::const_local:     return CONST_LOCAL;
    case tag::ref_of:
    case tag::address_of:      return REF_OF;
    case tag::init_struct:     return INIT_STRUCT;
    case tag::init_array:      return INIT_ARRAY;
    case tag::array_lit:       return ARRAY_LIT;
    case tag::range:           return RANGE;
    case tag::call:            return CALL;
    case tag::field_call:      return FIELD_CALL;
    case tag::builtin:         return BUILTIN;
    case tag::asm_expr:        return ASM;
    case tag::block:           return BLOCK;
    case tag::block_comptime:  return BLOCK_COMPTIME;
    case tag::loop:            return LOOP;
    case tag::for_iter:        return FOR_ITER;
    case tag::condbr:
    case tag::condbr_comptime: return CONDBR;
    case tag::eval_branch:     return EVAL_BRANCH;
    case tag::unwrap_branch:   return UNWRAP_BRANCH;
    case tag::match:           return MATCH;
    case tag::pat_capture:     return PAT_CAPTURE;
    case tag::pat_value:       return PAT_VALUE;
    case tag::pat_range:       return PAT_RANGE;
    case tag::pat_init:        return PAT_INIT;
    case tag::pat_call:        return PAT_CALL;
    case tag::unwrap:          return UNWRAP;
    case tag::func:            return FUNC;
    case tag::closure:         return CLOSURE;
    case tag::impl_decl:       return IMPL_DECL;
    case tag::attribute:       return ATTRIBUTE;
    default:                   return {};
    }
}

module_t::module_t() {
    // A shared empty body, so a decl or record with no body points at a zero length
    extra_.push_back(0);
}

auto module_t::namespace_of(u32 owner) const -> stdx::option<const namespace_info&> {
    if (owner == NO_INDEX) { return root_; }
    if (const auto it{namespaces_.find(owner)}; it != namespaces_.end()) { return it->second; }
    return stdx::none;
}

auto module_t::add(tag t, data d, source src) -> u32 {
    const auto index{instruction_count()};
    tags_.push_back(t);
    datas_.push_back(d);
    srcs_.push_back(src);
    return index;
}

auto module_t::reserve(source src) -> u32 { return add(tag::unreachable, data{.int_value = 0}, src); }

auto module_t::set(u32 index, tag t, data d) noexcept -> void {
    tags_[index]  = t;
    datas_[index] = d;
}

auto module_t::intern(std::string_view text) -> string_ref {
    if (const auto it{string_lut_.find(text)}; it != string_lut_.end()) { return it->second; }
    const string_ref added{static_cast<u32>(strings_.size()), static_cast<u32>(text.size())};
    strings_.append(text);
    string_lut_.emplace(std::string{text}, added);
    return added;
}

auto module_t::append_extra(gsl::span<const u32> words) -> u32 {
    const auto at{static_cast<u32>(extra_.size())};
    extra_.insert(extra_.end(), words.begin(), words.end());
    return at;
}

auto module_t::add_body(gsl::span<const u32> instructions) -> body_ref {
    if (instructions.empty()) { return EMPTY_BODY; }
    const auto at{static_cast<u32>(extra_.size())};
    extra_.push_back(static_cast<u32>(instructions.size()));
    extra_.insert(extra_.end(), instructions.begin(), instructions.end());
    return at;
}

auto module_t::add_decl(decl d) -> u32 {
    decls_.push_back(d);
    return static_cast<u32>(decls_.size() - 1);
}

auto module_t::add_import(import_info info) -> u32 {
    imports_.push_back(std::move(info));
    return static_cast<u32>(imports_.size() - 1);
}

auto module_t::add_namespace(namespace_info info) -> void {
    const auto owner{info.owner};
    namespaces_.insert_or_assign(owner, std::move(info));
}

auto record_reader::str_field() noexcept -> std::string_view {
    const string_ref s{word(), word()};
    return mod_.string(s);
}

auto record_reader::body_field() noexcept -> gsl::span<const u32> {
    const auto len{word()};
    const auto out{mod_.extra().subspan(at_, len)};
    at_ += len;
    return out;
}

auto record_reader::refs_field() noexcept -> gsl::span<const ref> {
    const auto words{body_field()};
    return {reinterpret_cast<const ref*>(words.data()), words.size()};
}

auto record_reader::words_field() noexcept -> gsl::span<const u32> { return body_field(); }

auto record_writer::str_field(std::string_view text) -> record_writer& {
    const auto s{mod_.intern(text)};
    word(s.start);
    return word(s.len);
}

auto record_writer::body_field(gsl::span<const u32> instructions) -> record_writer& {
    word(static_cast<u32>(instructions.size()));
    words_.insert(words_.end(), instructions.begin(), instructions.end());
    return *this;
}

auto record_writer::refs_field(gsl::span<const ref> refs) -> record_writer& {
    word(static_cast<u32>(refs.size()));
    for (const auto r : refs) { word(static_cast<u32>(r)); }
    return *this;
}

auto record_writer::words_field(gsl::span<const u32> words) -> record_writer& {
    return body_field(words);
}

auto record_writer::finish() -> u32 { return mod_.append_extra(words_); }

auto record_end(const module_t& mod, tag t, u32 at) -> u32 {
    return skip_record(mod, schema_of(t), at);
}

} // namespace ghoti::uir
