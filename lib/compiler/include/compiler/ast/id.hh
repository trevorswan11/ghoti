#pragma once

#include <bit>
#include <string_view>

#include <fmt/base.h>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/kind.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

namespace detail {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
constexpr auto INVALID_ID{static_cast<u64>(-1)};
#pragma clang diagnostic pop

} // namespace detail

// A compact id for all AST nodes
class node_id {
  public:
    constexpr node_id(node_kind kind, syntax::token_type_t type, u64 index) noexcept : raw_{} {
        ASSERT(index <= INDEX_MASK, "Requested node index is too large");
        raw_ |= static_cast<u64>(kind) << KIND_OFFSET;
        raw_ |= static_cast<u64>(type) << TOKEN_TYPE_OFFSET;
        raw_ |= index;
    }

    [[nodiscard]] constexpr auto get_kind() const noexcept -> node_kind {
        return static_cast<node_kind>((raw_ & KIND_MASK) >> KIND_OFFSET);
    }

    // Useful when the token type holds information about a node that is not known at creation
    constexpr auto set_token_type(syntax::token_type_t type) noexcept -> void {
        raw_ &= ~TOKEN_TYPE_MASK;
        raw_ |= static_cast<u64>(type) << TOKEN_TYPE_OFFSET;
    }

    [[nodiscard]] constexpr auto get_token_type() const noexcept -> syntax::token_type_t {
        return static_cast<syntax::token_type_t>((raw_ & TOKEN_TYPE_MASK) >> TOKEN_TYPE_OFFSET);
    }

    [[nodiscard]] constexpr auto get_index() const noexcept -> usize {
        return static_cast<usize>(raw_ & INDEX_MASK);
    }

    [[nodiscard]] static constexpr auto make_invalid() noexcept -> node_id {
        return node_id{detail::INVALID_ID};
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool {
        return raw_ != detail::INVALID_ID;
    }

    template <NodeData N> [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return get_kind() == node_kind_of<N>::value();
    }

    template <NodeData... Ns> [[nodiscard]] constexpr auto any() const noexcept -> bool {
        const auto kind{get_kind()};
        return ((kind == node_kind_of<Ns>::value()) || ...);
    }

    [[nodiscard]] auto display_name() const noexcept -> std::string_view;

  private:
    constexpr explicit node_id(u64 raw) noexcept : raw_{raw} {}

  private:
    static constexpr u64 KIND_MASK{0xFF00000000000000ULL};
    static constexpr u64 KIND_OFFSET{std::countr_zero(KIND_MASK)};
    static constexpr u64 TOKEN_TYPE_MASK{0x00FF000000000000ULL};
    static constexpr u64 TOKEN_TYPE_OFFSET{std::countr_zero(TOKEN_TYPE_MASK)};
    static constexpr u64 INDEX_MASK{0x0000FFFFFFFFFFFFULL};

  private:
    u64 raw_;

    template <node_kind... AllowedKinds> friend class handle;
};

#define MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(name, modifier)            \
    [[nodiscard]] constexpr auto is_##name() const noexcept -> bool { \
        if (is_value()) { return false; }                             \
        return underlying_ == (modifier);                             \
    }

class type_modifier {
  public:
    enum class modifier : u8 {
        VALUE,
        REF,
        MUT_REF,
        PTR,
        MUT_PTR,
        VOLATILE,
        MUT_VOLATILE,
    };

  public:
    constexpr type_modifier() noexcept = default;
    constexpr explicit type_modifier(modifier underlying) noexcept : underlying_{underlying} {}
    constexpr explicit type_modifier(u64 raw) noexcept : underlying_{static_cast<modifier>(raw)} {}
    explicit type_modifier(const syntax::token_t& tok) noexcept;

    [[nodiscard]] constexpr auto get_raw() const noexcept -> modifier { return underlying_; }

    // Whether or not the type is a 'value' type (no modifier), mutually exclusive result.
    [[nodiscard]] constexpr auto is_value() const noexcept -> bool {
        return underlying_ == modifier::VALUE;
    }

    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(mutable_ref, modifier::MUT_REF)
    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(const_ref, modifier::REF)
    [[nodiscard]] constexpr auto is_ref() const noexcept -> bool {
        return is_mutable_ref() || is_const_ref();
    }

    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(mutable_ptr, modifier::MUT_PTR)
    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(const_ptr, modifier::PTR)
    [[nodiscard]] constexpr auto is_ptr() const noexcept -> bool {
        return is_mutable_ptr() || is_const_ptr();
    }

    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(mutable_volatile, modifier::MUT_VOLATILE)
    MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY(const_volatile, modifier::VOLATILE)
    [[nodiscard]] constexpr auto is_volatile() const noexcept -> bool {
        return is_mutable_volatile() || is_const_volatile();
    }

    [[nodiscard]] constexpr auto operator==(const type_modifier& other) const noexcept
        -> bool = default;

    [[nodiscard]] constexpr explicit operator u64() const noexcept {
        return static_cast<u64>(underlying_);
    }

  private:
    modifier underlying_{modifier::VALUE};

    friend struct fmt::formatter<ghoti::ast::type_modifier>;
};

static_assert(magic_enum::enum_count<explicit_type_kind>() <= 0xF,
              "ExplicitTypeKind enum too large");
static_assert(magic_enum::enum_count<type_modifier::modifier>() <= 0xF,
              "TypeModifier enum too large");

// A compact id for all AST explicit types
class explicit_type_id {
  public:
    constexpr explicit_type_id(explicit_type_kind   kind,
                               type_modifier        mod,
                               syntax::token_type_t token_type,
                               u64                  index) noexcept
        : raw_{} {
        ASSERT(index <= INDEX_MASK, "Requested type index is too large");
        raw_ |= static_cast<u64>(kind) << KIND_OFFSET;
        raw_ |= static_cast<u64>(mod) << MODIFIER_OFFSET;
        raw_ |= static_cast<u64>(token_type) << TOKEN_TYPE_OFFSET;
        raw_ |= index;
    }

    [[nodiscard]] constexpr auto get_kind() const noexcept -> explicit_type_kind {
        return static_cast<explicit_type_kind>((raw_ & KIND_MASK) >> KIND_OFFSET);
    }

    [[nodiscard]] constexpr auto get_modifier() const noexcept -> type_modifier {
        return type_modifier{(raw_ & MODIFIER_MASK) >> MODIFIER_OFFSET};
    }

    [[nodiscard]] constexpr auto get_token_type() const noexcept -> syntax::token_type_t {
        return static_cast<syntax::token_type_t>((raw_ & TOKEN_TYPE_MASK) >> TOKEN_TYPE_OFFSET);
    }

    [[nodiscard]] constexpr auto get_index() const noexcept -> usize {
        return static_cast<usize>(raw_ & INDEX_MASK);
    }

    [[nodiscard]] static constexpr auto make_invalid() noexcept -> explicit_type_id {
        return explicit_type_id{detail::INVALID_ID};
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool {
        return raw_ != detail::INVALID_ID;
    }

    template <ExplicitTypeData N> [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return get_kind() == explicit_type_kind_of<N>::value();
    }

    template <ExplicitTypeData... Ns> [[nodiscard]] constexpr auto any() const noexcept -> bool {
        const auto kind{get_kind()};
        return ((kind == explicit_type_kind_of<Ns>::value()) || ...);
    }

  private:
    constexpr explicit explicit_type_id(const u64& raw) noexcept : raw_{raw} {}

  private:
    static constexpr u64 KIND_MASK{0xF000000000000000ULL};
    static constexpr u64 KIND_OFFSET{std::countr_zero(KIND_MASK)};
    static constexpr u64 MODIFIER_MASK{0x0F00000000000000ULL};
    static constexpr u64 MODIFIER_OFFSET{std::countr_zero(MODIFIER_MASK)};
    static constexpr u64 TOKEN_TYPE_MASK{0x00FF000000000000ULL};
    static constexpr u64 TOKEN_TYPE_OFFSET{std::countr_zero(TOKEN_TYPE_MASK)};
    static constexpr u64 INDEX_MASK{0x0000FFFFFFFFFFFFULL};

  private:
    u64 raw_;
};

#undef MAKE_MUTUALLY_EXCLUSIVE_TYPE_QUERY

} // namespace ghoti::ast

template <> struct stdx::nullable<ghoti::ast::node_id> {
    [[nodiscard]] static constexpr auto invalid() noexcept -> ghoti::ast::node_id {
        return ghoti::ast::node_id::make_invalid();
    }

    [[nodiscard]] static constexpr auto is_valid(ghoti::ast::node_id id) noexcept -> bool {
        return id.is_valid();
    }
};

template <> struct stdx::nullable<ghoti::ast::explicit_type_id> {
    [[nodiscard]] static constexpr auto invalid() noexcept -> ghoti::ast::explicit_type_id {
        return ghoti::ast::explicit_type_id::make_invalid();
    }

    [[nodiscard]] static constexpr auto is_valid(ghoti::ast::explicit_type_id id) noexcept -> bool {
        return id.is_valid();
    }
};
