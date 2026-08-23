#pragma once

#include <concepts>
#include <string_view>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/arena.hh>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/type.hh"
#include "compiler/module/module.hh"

namespace ghoti::sema {

enum class type_kind : u8 {
    POISON,
    I32,
    I64,
    ISIZE,
    U32,
    U64,
    USIZE,
    U8,
    BOOL,
    F32,
    F64,
    VOID_,
    UNDEFINED,
    NULLPTR,
    TYPE,
    SLICE,
    ARRAY,
    POINTER,
    REFERENCE,
    ENUM,
    STRUCT,
    UNION,
    FUNCTION,
    CLOSURE,
    LABEL,
    BLOCK,
    MATCH_ARM,
    MODULE,

    AUTO,
    OPAQUE,
    NORETURN,
};

[[nodiscard]] auto type_kind_display_name(type_kind kind) noexcept -> std::string_view;

[[nodiscard]] constexpr auto is_integer(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::I32:
    case type_kind::I64:
    case type_kind::ISIZE:
    case type_kind::U32:
    case type_kind::U64:
    case type_kind::USIZE:
    case type_kind::U8:    return true;
    default:               return false;
    }
}

[[nodiscard]] constexpr auto is_signed_integer(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::I32:
    case type_kind::I64:
    case type_kind::ISIZE: return true;
    default:               return false;
    }
}

[[nodiscard]] constexpr auto is_unsigned_integer(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::U32:
    case type_kind::U64:
    case type_kind::USIZE:
    case type_kind::U8:    return true;
    default:               return false;
    }
}

[[nodiscard]] constexpr auto is_float(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::F32:
    case type_kind::F64: return true;
    default:             return false;
    }
}

[[nodiscard]] constexpr auto is_numeric(type_kind kind) noexcept -> bool {
    return is_integer(kind) || is_float(kind);
}

[[nodiscard]] constexpr auto is_implicit_widenable(type_kind from, type_kind to) noexcept -> bool {
    switch (from) {
    case type_kind::U8:
        return to == type_kind::U32 || to == type_kind::U64 || to == type_kind::USIZE;
    case type_kind::U32: return to == type_kind::U64 || to == type_kind::USIZE;
    case type_kind::I32: return to == type_kind::I64 || to == type_kind::ISIZE;
    case type_kind::F32: return to == type_kind::F64;
    default:             return false;
    }
}

[[nodiscard]] constexpr auto is_value_type(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::VOID_:
    case type_kind::NORETURN:
    case type_kind::OPAQUE:
    case type_kind::BLOCK:
    case type_kind::MODULE:
    case type_kind::LABEL:
    case type_kind::MATCH_ARM:
    case type_kind::POISON:
    case type_kind::UNDEFINED: return false;
    default:                   return true;
    }
}

[[nodiscard]] constexpr auto is_aggregate(type_kind kind) noexcept -> bool {
    switch (kind) {
    case type_kind::STRUCT:
    case type_kind::UNION:
    case type_kind::SLICE:
    case type_kind::ARRAY:
    case type_kind::CLOSURE: return true;
    default:                 return false;
    }
}

class type;

[[nodiscard]] auto is_same_unqualified(const type& a, const type& b) noexcept -> bool;
[[nodiscard]] auto is_assignable(const type& src, const type& dest) noexcept -> bool;

namespace types {

struct unresolved {};
struct poison {};

using builtin_type = stdx::monostate;

struct slice {
    type& underlying;
    bool  null_terminated;
};

struct array {
    type& underlying;
    usize len;
    bool  null_terminated;
};

struct pointer {
    type& underlying;
};

struct reference {
    type& underlying;
};

struct enum_t {
    gsl::span<const ast::enum_expr::enumeration> ast_enumerations;
    bool                                         non_exhaustive;
    type&                                        underlying;
    gsl::span<type*>                             members;
    const mod::module&                           enclosing;

    [[nodiscard]] auto type_at(usize idx, type& object_type) const noexcept -> type& {
        // The index location entirely depends on the number of variants which always come first
        const auto enumeration_count{ast_enumerations.size()};
        ASSERT(idx < enumeration_count + members.size(), "Index exceeds enum's types");
        if (idx < enumeration_count) { return object_type; }
        return *members[idx - enumeration_count];
    }
};

struct union_t {
    gsl::span<type*>                        fields;
    gsl::span<const ast::union_expr::field> ast_fields;
    gsl::span<type*>                        members;
    const mod::module&                      enclosing;
    bool                                    is_untagged{false};

    // The index location entirely depends on the number of fields which always come first
    [[nodiscard]] auto type_at(usize idx) const noexcept -> type& {
        ASSERT(idx < fields.size() + members.size(), "Index exceeds union's types");
        if (idx < fields.size()) { return *fields[idx]; }
        return *members[idx - fields.size()];
    }
};

struct struct_t {
    gsl::span<type*>                         fields;
    gsl::span<const ast::struct_expr::field> ast_fields;
    gsl::span<type*>                         members;
    const mod::module&                       enclosing;
    bool                                     is_c_abi{false};
    bool                                     is_packed{false};
    gsl::span<u64>                           field_alignments;

    // The index location entirely depends on the number of fields which always come first
    [[nodiscard]] auto type_at(usize idx) const noexcept -> type& {
        ASSERT(idx < fields.size() + members.size(), "Index exceeds struct's types");
        if (idx < fields.size()) { return *fields[idx]; }
        return *members[idx - fields.size()];
    }
};

struct function {
    bool             has_self;
    gsl::span<type*> params;
    type&            return_type;
    bool             is_variadic{false};
};

// How a closure stores one of its captured free variables in its environment
enum class capture_mode : u8 {
    VALUE,   // `T`: Copyable  and never mutated by the closure body
    REF,     // `&T`: an aggregate that the closure body only ever reads
    MUT_REF, // `&mut T`: the closure body assigns to it or takes `&mut`/`^mut` of it
};

struct closure_capture {
    std::string_view name;
    type*            captured_type;
    type*            storage_type;
    capture_mode     mode;
};

struct closure_t {
    gsl::span<closure_capture> captures;
    type&                      signature;      // the public call shape: params/return as written
    type&                      impl_signature; // `signature` plus a synthetic `&mut self` param
};

struct module {
    mod::module& imported;
};

constexpr usize MAX_BUILTIN_PARAMS{4};
using builtin_params = stdx::fixed::vector<gsl::not_null<type*>, MAX_BUILTIN_PARAMS>;

struct builtin_function {
    builtin_params params;
    type&          return_type;
};

struct meta_type {
    type& instance;
};

struct deferred_call {
    const ast::call_expr& call;
};

struct deferred_array {
    const ast::explicit_array_type& array;
    type&                           underlying;
};

enum class mutability_modifiers : u8 {
    CONSTANT = 1 << 0,
    VOLATILE = 1 << 1,
};

MAKE_ENUM_OPERATORS(mutability_modifiers)

class key_t {
  public:
    template <typename... Markers>
    constexpr key_t(type_kind kind, mutability_modifiers mut, Markers&&... markers) noexcept
        : kind_{kind}, mut_{mut} {
        (..., markers_.combine(markers));
    }

    MAKE_GETTER(kind, type_kind)
    MAKE_GETTER(mut, mutability_modifiers)

    auto set_kind(type_kind kind) noexcept -> void { kind_ = kind; }
    auto set_mut(mutability_modifiers mut) noexcept -> void { mut_ = mut; }

    // This is a high quality hash for the purposes of `unordered_dense`
    [[nodiscard]] constexpr auto hash() const noexcept -> u64 {
        stdx::hasher h{std::to_underlying(kind_)};
        h.combine(mut_);
        h.combine(markers_);
        return h.finalize();
    }

    // Emplace a hashable marker into the accumulated markers.
    //
    // WARNING: Only imprint onto keys when you are creating a new type. Making a modification to
    // mutability only (before rehashing) should never require a fresh imprint of the previous type!
    // Failing to follow this will completely brick the type checking mechanism.
    template <typename Marker> constexpr auto imprint(const Marker& marker) noexcept -> void {
        markers_.combine(marker);
    }

    constexpr auto clear_markers() noexcept -> void { markers_ = {}; }

    [[nodiscard]] constexpr auto operator==(const key_t&) const noexcept -> bool = default;

  private:
    key_t() noexcept = default;

  private:
    type_kind            kind_;
    mutability_modifiers mut_;
    stdx::hasher         markers_;

    friend class sema::type;
};

namespace mut {

using types::mutability_modifiers;

constexpr auto MUTABLE{static_cast<types::mutability_modifiers>(0)};
constexpr auto CONSTANT{mutability_modifiers::CONSTANT};
constexpr auto VOLATILE{mutability_modifiers::VOLATILE};
constexpr auto CONSTANT_VOLATILE{CONSTANT | VOLATILE};

} // namespace mut

} // namespace types

} // namespace ghoti::sema

template <> struct ankerl::unordered_dense::hash<ghoti::sema::types::key_t> {
    using is_avalanching = void;
    using key_t          = ghoti::sema::types::key_t;
    [[nodiscard]] auto operator()(const key_t& key) const noexcept { return key.hash(); }
};

namespace ghoti::sema {

using arena_alloc = stdx::arena<stdx::sizes::kib(64UZ)>;

// A semantic type that is entirely owned by an arena of types
class type {
  public:
    using data_t = stdx::variant<types::unresolved,
                                 types::poison,
                                 types::builtin_type,
                                 types::slice,
                                 types::array,
                                 types::pointer,
                                 types::reference,
                                 types::enum_t,
                                 types::union_t,
                                 types::struct_t,
                                 types::module,
                                 types::function,
                                 types::closure_t,
                                 types::builtin_function,
                                 types::meta_type,
                                 types::deferred_call,
                                 types::deferred_array>;

  public:
    ~type() = default;

    type(const type&)                    = delete;
    auto operator=(const type&) -> type& = delete;
    type(type&&) noexcept                = delete;
    auto operator=(type&&) -> type&      = delete;

    MAKE_GETTER(key, const types::key_t&)
    MAKE_DEDUCING_GETTER(data)

    [[nodiscard]] auto get_kind() const noexcept -> type_kind { return key_.get_kind(); }

    // Intended for use on pass 1 only
    constexpr auto set_symbol_table_idx(usize idx) noexcept -> void {
        symbol_table_idx_.emplace(idx);
    }

    [[nodiscard]] auto has_symbol_table_idx() const noexcept -> bool {
        return symbol_table_idx_.has_value();
    }

    [[nodiscard]] auto get_symbol_table_idx() const noexcept { return *symbol_table_idx_; }
    [[nodiscard]] auto get_symbol_table_idx_opt() const noexcept { return symbol_table_idx_; }

    [[nodiscard]] auto is_resolved() const noexcept -> bool {
        return !data_.is<types::unresolved>();
    }

    auto unresolve() noexcept -> void { data_.emplace<types::unresolved>(); }
    template <typename Resolvee, typename... Args> auto resolve(Args&&... args) noexcept -> void {
        data_.emplace<Resolvee>(std::forward<Args>(args)...);
    }

    // Resolves only if not already resolved, returning true if modified
    template <typename Resolvee, typename... Args>
    auto resolve_if(Args&&... args) noexcept -> bool {
        if (is_resolved()) { return false; }
        data_.emplace<Resolvee>(std::forward<Args>(args)...);
        return true;
    }

    // Returns the memory address of the Type for a Key's hash
    [[nodiscard]] constexpr auto hash() const noexcept -> u64 {
        return reinterpret_cast<u64>(this);
    }

    [[nodiscard]] constexpr auto is_poison() const noexcept -> bool {
        return key_.get_kind() == type_kind::POISON;
    }

    [[nodiscard]] constexpr auto is_constant() const noexcept -> bool {
        return static_cast<bool>(key_.get_mut() & types::mut::CONSTANT);
    }

    [[nodiscard]] constexpr auto is_volatile() const noexcept -> bool {
        return static_cast<bool>(key_.get_mut() & types::mut::VOLATILE);
    }

    [[nodiscard]] constexpr auto operator==(const type& other) const noexcept -> bool {
        return this == &other;
    }

  private:
    // This should only be used when allocating an immediately-to-be-filled span
    type() noexcept = default;
    explicit type(types::key_t key) noexcept : key_{key} {}

  private:
    types::key_t   key_;
    stdx::opt_size symbol_table_idx_;
    data_t         data_;

    // Initialization is restricted to the pool's arena exclusively
    friend arena_alloc;
};

static_assert(stdx::TriviallyDestructible<type>);

// All associated type lifetimes are tied to the pool
class type_pool {
  public:
    explicit type_pool(arena_alloc& arena) noexcept : arena_{arena} {}
    ~type_pool() = default;

    MAKE_MOVE_CONSTRUCTABLE_ONLY(type_pool)

    [[nodiscard]] auto arena() noexcept -> auto& { return arena_; }

    // Gets a type by its key or emplace's it into the internal cache
    [[nodiscard]] auto operator[](const types::key_t& key) -> gsl::not_null<type*> {
        return get_or_emplace(key);
    }

    // Allocate a quasi-contiguous span of types with the provided keys
    template <std::same_as<types::key_t>... Keys>
    [[nodiscard]] auto get_many(Keys&&... keys) noexcept -> gsl::span<type*> {
        auto  types{arena_.make_span<type*>(sizeof...(Keys))};
        usize i{0};
        (..., (types[i++] = get_or_emplace(keys)));
        return types;
    }

    // Allocates the requested number of types but does not initialize any data
    [[nodiscard]] auto get_many_unsafe(usize count) noexcept -> gsl::span<type*>;

    // Allocate a quasi-contiguous span of types with the same types
    [[nodiscard]] auto get_many(usize count, type& common_type) noexcept -> gsl::span<type*>;

    // Allocate a quasi-contiguous span of types with the same key types
    [[nodiscard]] auto get_many(usize count, types::key_t common_key) noexcept -> gsl::span<type*> {
        return get_many(count, *get_or_emplace(common_key));
    }

    [[nodiscard]] auto strip_const(const type& t) -> gsl::not_null<type*>;
    [[nodiscard]] auto strip_volatile(const type& t) -> gsl::not_null<type*>;

    // Returns t's const or mutable twin, whichever `is_const` asks for; t itself if it's
    // already in that state
    [[nodiscard]] auto with_const(const type& t, bool is_const) -> gsl::not_null<type*>;

  private:
    auto get_or_emplace(const types::key_t& key) -> gsl::not_null<type*>;

  private:
    arena_alloc&                                      arena_;
    ankerl::unordered_dense::map<types::key_t, type*> cache_;
};

} // namespace ghoti::sema

template <> struct ankerl::unordered_dense::hash<ghoti::sema::type> {
    using is_avalanching = void;
    using type_t         = ghoti::sema::type;
    [[nodiscard]] auto operator()(const type_t& type) const noexcept { return type.hash(); }
};
