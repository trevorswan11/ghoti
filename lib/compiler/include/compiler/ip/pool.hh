#pragma once

#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/arena.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/type.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::ip {

// Values that carry nothing but their type
struct void_value {
    [[nodiscard]] constexpr auto operator==(const void_value&) const noexcept -> bool = default;
};
struct undefined_value {
    [[nodiscard]] constexpr auto operator==(const undefined_value&) const noexcept
        -> bool = default;
};

// Every bit zero, whatever the type
struct zero_value {
    [[nodiscard]] constexpr auto operator==(const zero_value&) const noexcept -> bool = default;
};
struct nullptr_value {
    [[nodiscard]] constexpr auto operator==(const nullptr_value&) const noexcept -> bool = default;
};
struct poison_value {
    [[nodiscard]] constexpr auto operator==(const poison_value&) const noexcept -> bool = default;
};

// A `type` operand that names no type
struct no_type_value {
    [[nodiscard]] constexpr auto operator==(const no_type_value&) const noexcept -> bool = default;
};

// A struct's fields or an array's elements, in declaration order
struct aggregate_value {
    std::vector<stdx::option<index>> elements; // A struct field nothing set is none
    [[nodiscard]] auto operator==(const aggregate_value&) const noexcept -> bool = default;
};

struct enum_value {
    std::string        name;
    i64                value{0};
    [[nodiscard]] auto operator==(const enum_value&) const noexcept -> bool = default;
};

struct union_value {
    std::string        active_field;
    std::vector<index> payload; // empty for a payloadless variant
    [[nodiscard]] auto operator==(const union_value&) const noexcept -> bool = default;
};

// The address of a symbol, and what it points at when that is known at compile time
struct address_value {
    std::string        symbol;
    std::vector<index> pointee;
    [[nodiscard]] auto operator==(const address_value&) const noexcept -> bool = default;
};

// An address in compile-time memory
struct memory_value {
    u32              alloc{0};
    std::vector<u32> path; // The elements leading into the allocation
    // A slice's elements of the array `path` leads to
    u64                offset{0};
    u64                len{0};
    [[nodiscard]] auto operator==(const memory_value&) const noexcept -> bool = default;
};

// A `dyn` object's data and vtable symbols, either of which may be empty for a null pointer
struct dyn_value {
    std::string        data_symbol;
    std::string        vtable_symbol;
    [[nodiscard]] auto operator==(const dyn_value&) const noexcept -> bool = default;
};

// An integer keeps the width it was written with, so it reads back exactly as it went in
using value_data = stdx::variant<i64,
                                 u64,
                                 i128,
                                 u128,
                                 big_int,
                                 f128,
                                 bool,
                                 std::string,
                                 aggregate_value,
                                 enum_value,
                                 union_value,
                                 address_value,
                                 memory_value,
                                 dyn_value,
                                 void_value,
                                 undefined_value,
                                 zero_value,
                                 nullptr_value,
                                 poison_value,
                                 no_type_value>;

struct value {
    value_data                data;
    stdx::option<sema::type&> type;
};

// One per compilation: every type and every interned value, each named by its `index`.
// A type's own index is also its value as a `type`
class pool {
  public:
    explicit pool(ghoti::arena& arena) noexcept : arena_{arena} {}
    ~pool() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(pool)

    [[nodiscard]] auto arena() noexcept -> ghoti::arena& { return arena_; }
    [[nodiscard]] auto size() const noexcept -> usize { return entries_.size(); }

    // The type `key` names, created unresolved the first time it is asked for
    [[nodiscard]] auto type_for(const sema::types::key_t& key) -> gsl::not_null<sema::type*>;
    [[nodiscard]] auto type_at(index i) const noexcept -> sema::type&;
    [[nodiscard]] auto is_type(index i) const noexcept -> bool;

    // The value `data` of `type`, interned once. A float is interned as the bits it was given
    [[nodiscard]] auto intern(value_data data, stdx::option<sema::type&> type = stdx::none)
        -> index;
    [[nodiscard]] auto value_at(index i) const noexcept -> const value&;

  private:
    struct entry {
        stdx::option<sema::type&> type_entry; // set for a type, none for a value
        u32                       value_slot{0};
    };

    using type_key_map_t  = ankerl::unordered_dense::map<sema::types::key_t, index>;
    using value_key_map_t = ankerl::unordered_dense::
        map<std::string, index, stdx::string_transparent_hash, stdx::string_transparent_eq>;

  private:
    [[nodiscard]] auto next_index() const noexcept -> index {
        return index{static_cast<u32>(entries_.size())};
    }

  private:
    ghoti::arena&      arena_;
    std::vector<entry> entries_;
    std::vector<value> values_;
    type_key_map_t     type_keys_;
    value_key_map_t    value_keys_;
};

} // namespace ghoti::ip
