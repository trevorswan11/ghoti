#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"

namespace ghoti::gir {

enum class instruction_kind : u8 {
    // Memory
    ALLOCA,
    LOAD,
    STORE,
    GET_ELEMENT_PTR,
    ADDRESS_OF,
    DEREF,
    GLOBAL_ADDR,

    // Binary arithmetic & bitwise
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    AND,
    OR,
    XOR,
    SHL,
    SHR,

    // Unary
    NEG,
    NOT,
    BITNOT,

    // Comparisons
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,

    // Casts and conversions
    WIDEN_CAST,
    BIT_CAST,
    PTR_CAST,
    INT_FROM_PTR,
    PTR_FROM_INT,

    // Calls
    CALL,
    BUILTIN_CALL,
    INLINE_ASM,

    // Constants
    CONSTANT,

    // Terminators
    RET,
    GOTO,
    COND_GOTO,
    UNREACHABLE,
};

[[nodiscard]] auto instruction_kind_name(instruction_kind kind) noexcept -> std::string_view;

[[nodiscard]] constexpr auto is_terminator(instruction_kind kind) noexcept -> bool {
    switch (kind) {
    case instruction_kind::RET:
    case instruction_kind::GOTO:
    case instruction_kind::COND_GOTO:
    case instruction_kind::UNREACHABLE: return true;
    default:                            return false;
    }
}

enum class linkage : u8 {
    INTERNAL,
    PUBLIC,
    EXPORT,
    EXTERN,
};

enum class local_kind : u8 {
    TEMPORARY,
    PARAMETER,
    ALLOCA,
    GLOBAL,
};

class local_id {
  public:
    constexpr local_id() noexcept = default;
    constexpr local_id(usize index, local_kind kind) noexcept : index_{index}, kind_{kind} {}

    [[nodiscard]] static constexpr auto make_temp(usize index) noexcept -> local_id {
        return local_id{index, local_kind::TEMPORARY};
    }

    [[nodiscard]] static constexpr auto make_param(usize index) noexcept -> local_id {
        return local_id{index, local_kind::PARAMETER};
    }

    [[nodiscard]] static constexpr auto make_alloca(usize index) noexcept -> local_id {
        return local_id{index, local_kind::ALLOCA};
    }

    [[nodiscard]] static constexpr auto make_global(usize index) noexcept -> local_id {
        return local_id{index, local_kind::GLOBAL};
    }

    [[nodiscard]] constexpr auto get_index() const noexcept -> usize { return index_; }
    [[nodiscard]] constexpr auto get_kind() const noexcept -> local_kind { return kind_; }

    [[nodiscard]] constexpr auto is_temp() const noexcept -> bool {
        return kind_ == local_kind::TEMPORARY;
    }
    [[nodiscard]] constexpr auto is_param() const noexcept -> bool {
        return kind_ == local_kind::PARAMETER;
    }
    [[nodiscard]] constexpr auto is_alloca() const noexcept -> bool {
        return kind_ == local_kind::ALLOCA;
    }
    [[nodiscard]] constexpr auto is_global() const noexcept -> bool {
        return kind_ == local_kind::GLOBAL;
    }

    [[nodiscard]] constexpr auto operator==(const local_id&) const noexcept -> bool = default;

  private:
    usize      index_{0};
    local_kind kind_{local_kind::TEMPORARY};
};

struct void_val {
    [[nodiscard]] constexpr auto operator==(const void_val&) const noexcept -> bool = default;
};
struct undefined_val {
    [[nodiscard]] constexpr auto operator==(const undefined_val&) const noexcept -> bool = default;
};
struct nullptr_val {
    [[nodiscard]] constexpr auto operator==(const nullptr_val&) const noexcept -> bool = default;
};

struct value {
    using data_t = stdx::variant<local_id,
                                 i64,
                                 u64,
                                 f64,
                                 bool,
                                 std::string,
                                 stdx::option<sema::type&>,
                                 void_val,
                                 undefined_val,
                                 nullptr_val>;

    data_t                    data{void_val{}};
    stdx::option<sema::type&> type{stdx::none};

    constexpr value() noexcept = default;
    constexpr value(data_t val, stdx::option<sema::type&> t = stdx::none) noexcept
        : data{std::move(val)}, type{t} {}

    template <typename T> [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return data.is<T>();
    }

    template <typename T, typename Self>
    [[nodiscard]] constexpr auto as(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).data.template as<T>();
    }

    template <typename T>
    [[nodiscard]] constexpr auto as_opt(this auto& self) noexcept -> decltype(auto) {
        return self.data.template as_opt<T>();
    }

    [[nodiscard]] constexpr auto operator==(const value& other) const noexcept -> bool = default;
};

enum class segment_id : usize {};

struct inline_asm {
    std::string        tmpl;
    std::string        constraints;
    std::vector<value> output_addrs;
    bool               is_volatile{false};
    bool               is_noreturn{false};
    bool               align_stack{false};
    bool               intel_dialect{false};
    bool               has_result_slot{false};

    [[nodiscard]] auto result_count() const noexcept -> usize {
        return has_result_slot ? 1 : output_addrs.size();
    }
};

struct instruction {
    instruction_kind              kind{instruction_kind::UNREACHABLE};
    stdx::option<sema::type&>     type{stdx::none};
    stdx::option<local_id>        result{stdx::none};
    std::vector<value>            operands{};
    stdx::option<segment_id>      target_segment{stdx::none};
    stdx::option<segment_id>      true_segment{stdx::none};
    stdx::option<segment_id>      false_segment{stdx::none};
    stdx::option<std::string>     callee_name{stdx::none};
    stdx::option<source_location> location{stdx::none};
    stdx::option<inline_asm>      asm_info{stdx::none};
    bool                          is_const{false};
    bool                          is_initializer{false};
    bool                          is_checked{false};

    [[nodiscard]] auto is_terminator() const noexcept -> bool { return gir::is_terminator(kind); }
    [[nodiscard]] auto has_result() const noexcept -> bool { return result.has_value(); }
    [[nodiscard]] auto is_volatile() const noexcept -> bool {
        return (type && type->is_volatile()) ||
               (!operands.empty() && operands.front().type && operands.front().type->is_volatile());
    }
};

} // namespace ghoti::gir

template <> struct ankerl::unordered_dense::hash<ghoti::gir::local_id> {
    using is_avalanching = void;
    using local_id       = ghoti::gir::local_id;
    [[nodiscard]] auto operator()(const local_id& id) const noexcept {
        return stdx::hasher{id.get_index()}.combine(id.get_kind()).finalize();
    }
};
