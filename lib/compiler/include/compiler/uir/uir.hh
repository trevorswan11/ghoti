#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/span>
#include <stdx/enum.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/id.hh"

namespace ghoti::uir {

enum class tag : u8 {
#define UIR_TAG(name, payload) name,
#include "compiler/uir/tags.def"
#undef UIR_TAG
};

enum class payload_kind : u8 {
    NONE,
    UN,
    BIN,
    STR,
    INT,
    PL,
};

[[nodiscard]] auto payload_of(tag t) noexcept -> payload_kind;
[[nodiscard]] auto tag_name(tag t) noexcept -> std::string_view;

// Operands every module shares, so they need no instruction of their own
#define FOREACH_UIR_WELL_KNOWN(X) \
    X(none)                       \
    X(u8_type)                    \
    X(i8_type)                    \
    X(u16_type)                   \
    X(i16_type)                   \
    X(u32_type)                   \
    X(i32_type)                   \
    X(u64_type)                   \
    X(i64_type)                   \
    X(u128_type)                  \
    X(i128_type)                  \
    X(usize_type)                 \
    X(isize_type)                 \
    X(f16_type)                   \
    X(f32_type)                   \
    X(f64_type)                   \
    X(f80_type)                   \
    X(f128_type)                  \
    X(bool_type)                  \
    X(void_type)                  \
    X(type_type)                  \
    X(auto_type)                  \
    X(opaque_type)                \
    X(noreturn_type)              \
    X(comptime_int_type)          \
    X(comptime_float_type)        \
    X(undefined_type)             \
    X(nullptr_type)               \
    X(c_char_type)                \
    X(c_short_type)               \
    X(c_ushort_type)              \
    X(c_int_type)                 \
    X(c_uint_type)                \
    X(c_long_type)                \
    X(c_ulong_type)               \
    X(c_longlong_type)            \
    X(c_ulonglong_type)           \
    X(c_longdouble_type)          \
    X(bool_true)                  \
    X(bool_false)                 \
    X(void_value)                 \
    X(undefined)                  \
    X(nullptr_value)              \
    X(zero_usize)                 \
    X(one_usize)                  \
    X(empty_slice)                \
    X(builtin_module)

// clang-format off
enum class well_known : u32 {
#define UIR_WELL_KNOWN_ENUM(name) name,
    FOREACH_UIR_WELL_KNOWN(UIR_WELL_KNOWN_ENUM)
#undef UIR_WELL_KNOWN_ENUM
};
// clang-format on

#define UIR_WELL_KNOWN_COUNT_X(name) +1
inline constexpr u32 WELL_KNOWN_COUNT{0 FOREACH_UIR_WELL_KNOWN(UIR_WELL_KNOWN_COUNT_X)};
#undef UIR_WELL_KNOWN_COUNT_X

[[nodiscard]] auto well_known_name(well_known w) noexcept -> std::string_view;

// An operand: a well-known constant below `WELL_KNOWN_COUNT`, else an instruction index offset by
// it
enum class ref : u32 {};

[[nodiscard]] constexpr auto to_ref(well_known w) noexcept -> ref {
    return ref{static_cast<u32>(w)};
}
[[nodiscard]] constexpr auto inst_ref(u32 index) noexcept -> ref {
    return ref{index + WELL_KNOWN_COUNT};
}
[[nodiscard]] constexpr auto is_none(ref r) noexcept -> bool {
    return r == to_ref(well_known::none);
}
[[nodiscard]] constexpr auto is_inst(ref r) noexcept -> bool {
    return static_cast<u32>(r) >= WELL_KNOWN_COUNT;
}
[[nodiscard]] constexpr auto inst_index(ref r) noexcept -> u32 {
    return static_cast<u32>(r) - WELL_KNOWN_COUNT;
}
[[nodiscard]] constexpr auto as_well_known(ref r) noexcept -> stdx::option<well_known> {
    if (is_inst(r)) { return stdx::none; }
    return static_cast<well_known>(static_cast<u32>(r));
}

struct string_ref {
    u32 start{0};
    u32 len{0};
};

union data {
    struct {
        ref operand;
        u32 aux;
    } un;
    struct {
        ref lhs;
        ref rhs;
    } bin;
    string_ref str;
    u64        int_value;
    struct {
        u32 extra;
        u32 len;
    } pl;
};
static_assert(sizeof(data) == 8);

// What an instruction was lowered from
using source = stdx::variant<ast::node_id, ast::explicit_type_id>;

// The kinds of field an `extra` record holds, in the order its schema lists them
enum class field_kind : u8 {
    REF,
    U32,
    STR,   // two words: start and length in the string table
    BODY,  // a length, then that many instruction indices
    REFS,  // a length, then that many refs
    WORDS, // a length, then that many raw words
    DECL,  // a declaration index
    LIST,  // a length, then that many records of `items`
};

struct field {
    field_kind                        kind;
    std::string_view                  name;
    gsl::span<const field>            items{};      // LIST only
    gsl::span<const std::string_view> flag_names{}; // a U32 printed as a set of named bits
    std::string_view (*describe)(u32){nullptr};     // a U32 printed as an enumerator
};

// The layout of a `PL` tag's record. Empty for every other tag
[[nodiscard]] auto schema_of(tag t) noexcept -> gsl::span<const field>;

namespace flags {

enum : u32 {
    // int_lit
    INT_SIGNED = 1U << 0,
    INT_SIZE   = 1U << 1,
    INT_CHAR   = 1U << 2,
    INT_BIG    = 1U << 3,

    // ptr_type, ref_type, slice_type, array_type, ref_of, address_of
    MUT        = 1U << 0,
    VOLATILE   = 1U << 1,
    POLY       = 1U << 2,
    SENTINEL   = 1U << 3,
    PLACE      = 1U << 4, // the operand is a place, not a value
    LEN_ABSENT = 1U << 5, // `[]T{...}` sized by its items

    // fn_type, func
    VARIADIC      = 1U << 0,
    EXTERN_FN     = 1U << 1,
    DYN_FN        = 1U << 2,
    IMPL_FN       = 1U << 3,
    EXPLICIT_CONV = 1U << 4,
    MOVE          = 1U << 5,
    TYPE_EXPR     = 1U << 6, // a bodyless signature used as a type
    TESTING       = 1U << 7, // `@[testing]`

    // struct_decl, union_decl
    AGG_EXTERN = 1U << 0,
    AGG_PACKED = 1U << 1,

    // enum_decl
    NON_EXHAUSTIVE = 1U << 0,

    // param
    PARAM_COMPTIME         = 1U << 0,
    PARAM_COMPTIME_WRITTEN = 1U << 1,
    PARAM_PACK             = 1U << 2,
    PARAM_SELF             = 1U << 3,
    PARAM_DISCARDED        = 1U << 4,
    PARAM_WRITTEN          = 1U << 5,

    // slice, range
    INCLUSIVE = 1U << 0,

    // loop, for_iter
    UNROLL   = 1U << 0,
    DO_WHILE = 1U << 1,
    UNWRAPS  = 1U << 2, // `while (x) |v|`
    HAS_COND = 1U << 3,

    // match
    MATCH_COMPTIME = 1U << 0,
    MATCH_BINDING  = 1U << 1, // a destructuring's pattern

    // field public, struct field
    PUBLIC = 1U << 0,
};

} // namespace flags

enum class unwrap_kind : u32 {
    PROPAGATE,
    ASSERT,
};
enum class union_tag : u32 {
    NONE,
    INFERRED,
    EXPLICIT,
};
inline constexpr u32 NO_INDEX{~u32{0}};

enum class decl_kind : u8 {
    VALUE,
    IMPORT,
    TEST,
    IMPL,
    COMPTIME,
};

enum class decl_flags : u32 {
    NONE        = 0,
    PUBLIC      = 1U << 0,
    EXTERN      = 1U << 1,
    EXPORT      = 1U << 2,
    THREADLOCAL = 1U << 3,
    WEAK        = 1U << 4,
    CONSTANT    = 1U << 5,
    LET         = 1U << 6,
    MUT         = 1U << 7,
    COMPTIME    = 1U << 8,
    SYNTHESIZED = 1U << 9,
};
MAKE_ENUM_OPERATORS(decl_flags)

// A body stored in `extra`: its length, then its instruction indices
using body_ref = u32;
inline constexpr body_ref EMPTY_BODY{0}; // `extra[0]` is a zero length the module reserves

struct decl {
    decl_kind                  kind{decl_kind::VALUE};
    string_ref                 name{};
    stdx::option<ast::node_id> node{};
    decl_flags                 flags{decl_flags::NONE};
    body_ref                   type_body{EMPTY_BODY};
    body_ref                   value_body{EMPTY_BODY};
    body_ref                   attrs_body{EMPTY_BODY};
    string_ref                 extern_target{};
    string_ref                 link_name{};
    u32                        import_index{NO_INDEX};
    u32 owner{NO_INDEX}; // the namespace's instruction, NO_INDEX for the module

    [[nodiscard]] auto has(decl_flags flag) const noexcept -> bool {
        return static_cast<bool>(flags & flag);
    }
};

// An aggregate's, impl's, or the module's own declarations, in source order
struct namespace_info {
    u32              owner{NO_INDEX};
    std::vector<u32> decls;
};

struct import_info {
    std::string                         spec{}; // a file path, or a library module's name
    bool                                is_library{false};
    ast::node_id                        node;
    stdx::option<std::filesystem::path> path{}; // where it loaded from, when it did
};

class module_t {
  public:
    module_t();
    ~module_t() = default;
    MAKE_MOVE_ONLY(module_t)

    [[nodiscard]] auto instruction_count() const noexcept -> u32 {
        return static_cast<u32>(tags_.size());
    }
    [[nodiscard]] auto tag_of(u32 index) const noexcept -> tag { return tags_[index]; }
    [[nodiscard]] auto data_of(u32 index) const noexcept -> const data& { return datas_[index]; }
    [[nodiscard]] auto src_of(u32 index) const noexcept -> const source& { return srcs_[index]; }
    [[nodiscard]] auto string(string_ref s) const noexcept -> std::string_view {
        return stdx::string::substr(strings_, s.start, s.len);
    }
    [[nodiscard]] auto extra() const noexcept -> gsl::span<const u32> { return extra_; }
    [[nodiscard]] auto body(body_ref at) const noexcept -> gsl::span<const u32> {
        return gsl::span<const u32>{extra_}.subspan(at + 1, extra_[at]);
    }

    [[nodiscard]] auto decls() const noexcept -> gsl::span<const decl> { return decls_; }
    [[nodiscard]] auto decl_at(u32 index) const noexcept -> const decl& { return decls_[index]; }
    [[nodiscard]] auto imports() const noexcept -> gsl::span<const import_info> { return imports_; }
    [[nodiscard]] auto root_namespace() const noexcept -> const namespace_info& { return root_; }
    [[nodiscard]] auto namespace_of(u32 owner) const -> stdx::option<const namespace_info&>;

    // Appends an instruction and returns its index
    auto add(tag t, data d, source src) -> u32;
    // Appends a placeholder to fill with `set` once its payload is known, so bodies can refer to it
    auto reserve(source src) -> u32;
    auto set(u32 index, tag t, data d) noexcept -> void;
    auto intern(std::string_view text) -> string_ref;
    // Appends `words` to `extra` and returns where they start
    auto append_extra(gsl::span<const u32> words) -> u32;
    auto add_body(gsl::span<const u32> instructions) -> body_ref;

    auto               add_decl(decl d) -> u32;
    [[nodiscard]] auto decl_at_mut(u32 index) noexcept -> decl& { return decls_[index]; }
    auto               add_import(import_info info) -> u32;
    auto               add_namespace(namespace_info info) -> void;
    auto               root_namespace_mut() noexcept -> namespace_info& { return root_; }

  private:
    std::vector<tag>    tags_;
    std::vector<data>   datas_;
    std::vector<source> srcs_;
    std::vector<u32>    extra_;
    std::string         strings_;
    ankerl::unordered_dense::
        map<std::string, string_ref, stdx::string_transparent_hash, stdx::string_transparent_eq>
                                                      string_lut_;
    std::vector<decl>                                 decls_;
    std::vector<import_info>                          imports_;
    namespace_info                                    root_;
    ankerl::unordered_dense::map<u32, namespace_info> namespaces_;
};

// Reads an `extra` record field by field, in schema order
class record_reader {
  public:
    record_reader(const module_t& mod, u32 at) noexcept : mod_{mod}, at_{at} {}

    [[nodiscard]] auto ref_field() noexcept -> ref { return static_cast<ref>(word()); }
    [[nodiscard]] auto u32_field() noexcept -> u32 { return word(); }
    [[nodiscard]] auto str_field() noexcept -> std::string_view;
    [[nodiscard]] auto body_field() noexcept -> gsl::span<const u32>;
    [[nodiscard]] auto refs_field() noexcept -> gsl::span<const ref>;
    [[nodiscard]] auto words_field() noexcept -> gsl::span<const u32>;
    [[nodiscard]] auto count() noexcept -> u32 { return word(); }
    [[nodiscard]] auto position() const noexcept -> u32 { return at_; }

  private:
    [[nodiscard]] auto word() noexcept -> u32 { return mod_.extra()[at_++]; }

  private:
    const module_t& mod_;
    u32           at_;
};

// Builds an `extra` record field by field, in schema order, then appends it
class record_writer {
  public:
    explicit record_writer(module_t& mod) noexcept : mod_{mod} {}

    auto ref_field(ref r) -> record_writer& { return word(static_cast<u32>(r)); }
    auto u32_field(u32 value) -> record_writer& { return word(value); }
    auto str_field(std::string_view text) -> record_writer&;
    auto body_field(gsl::span<const u32> instructions) -> record_writer&;
    auto refs_field(gsl::span<const ref> refs) -> record_writer&;
    auto words_field(gsl::span<const u32> words) -> record_writer&;
    auto count(usize n) -> record_writer& { return word(static_cast<u32>(n)); }

    // Appends the record to `extra` and returns where it starts
    [[nodiscard]] auto finish() -> u32;

  private:
    auto word(u32 value) -> record_writer& {
        words_.push_back(value);
        return *this;
    }

  private:
    module_t&          mod_;
    std::vector<u32> words_;
};

// Walks a record by its schema, returning where it ends; used to check what a writer produced
[[nodiscard]] auto record_end(const module_t& mod, tag t, u32 at) -> u32;

} // namespace ghoti::uir
