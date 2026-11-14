#pragma once

#include <concepts>
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

#include "compiler/ast/expression.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/big_int.hh"
#include "support/diagnostic.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

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
    INT_CAST,
    TRUNC_CAST,
    BIT_CAST,
    PTR_CAST,
    INT_FROM_PTR,
    PTR_FROM_INT,

    // `{ctx, code}` for an erased `fn(...)`; `callee_name` is the code, else a thin-fn trampoline
    MAKE_CALLABLE,

    // Vectors
    SPLAT,        // every lane is operand 0
    EXTRACT_LANE, // lane operand 1 of vector operand 0
    INSERT_LANE,  // vector operand 0 with lane operand 1 set to operand 2
    REPACK,       // `[N]T` <-> `@Vector(N, T)` by value
    SHUFFLE,      // lanes of operands 0 and 1 picked by the mask in the remaining operands
    SELECT,       // per lane, operand 0 ? operand 1 : operand 2
    REDUCE,       // operand 0's lanes folded by `lane_op` (a `builtin.ReduceOp`)

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

enum class reduce_op : u8 { // Mirrors `builtin.ReduceOp`
    ADD,
    MUL,
    AND,
    OR,
    XOR,
    MIN,
    MAX,
};

// The operator folding two lanes; integer sums and products wrap, as the vector instructions do
[[nodiscard]] constexpr auto reduce_token(reduce_op op, bool is_float) -> syntax::token_type_t {
    switch (op) {
    case reduce_op::ADD:
        return is_float ? syntax::token_type_t::PLUS : syntax::token_type_t::PLUS_PERCENT;
    case reduce_op::MUL:
        return is_float ? syntax::token_type_t::STAR : syntax::token_type_t::STAR_PERCENT;
    case reduce_op::AND: return syntax::token_type_t::BW_AND;
    case reduce_op::OR:  return syntax::token_type_t::BW_OR;
    case reduce_op::XOR: return syntax::token_type_t::CARET;
    case reduce_op::MIN: return syntax::token_type_t::LT;
    case reduce_op::MAX: return syntax::token_type_t::GT;
    default:             UNREACHABLE("Unknown reduce op");
    }
}

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

using void_val      = ip::void_value;
using undefined_val = ip::undefined_value;
// A zero-valued placeholder for an arbitrary type, lowered via `llvm::Constant::getNullValue`
using zero_val    = ip::zero_value;
using nullptr_val = ip::nullptr_value;

// An operand: a local, or a constant the compilation's intern pool holds
struct value {
    using data_t = stdx::variant<local_id, ip::index>;

    data_t                    data{local_id{}};
    stdx::option<sema::type&> type{stdx::none};
    // Set on an untyped `@sin(...)`-style result until it takes a concrete float type
    stdx::option<math_origin> origin{stdx::none};

    constexpr value() noexcept = default;
    constexpr explicit value(data_t                    val,
                             stdx::option<sema::type&> t        = stdx::none,
                             stdx::option<math_origin> computed = stdx::none)
        : data{val}, type{t}, origin{computed} {}

    // `payload` interned as a constant of type `t`. A float is rounded into `t`'s format first
    [[nodiscard]] static auto constant(ip::pool&                 pool,
                                       ip::value_data            payload,
                                       stdx::option<sema::type&> t        = stdx::none,
                                       stdx::option<math_origin> computed = stdx::none) -> value;
    // The type `denoted` used as a value, or the `type` operand that names no type
    [[nodiscard]] static auto type_constant(ip::pool&                 pool,
                                            stdx::option<sema::type&> denoted,
                                            stdx::option<sema::type&> t = stdx::none) -> value;
    // This operand as a `t`: a local is just relabeled, a constant interned again as a `t`
    [[nodiscard]] auto retyped(ip::pool&                 pool,
                               stdx::option<sema::type&> t,
                               stdx::option<math_origin> computed = stdx::none) const -> value;

    // A constant's payload is read through the pool with `literal_opt`, never through these
    template <typename T>
        requires std::same_as<T, local_id> || std::same_as<T, ip::index>
    [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return data.is<T>();
    }
    template <typename T>
        requires std::same_as<T, local_id> || std::same_as<T, ip::index>
    [[nodiscard]] constexpr auto as_opt() const noexcept {
        return data.template as_opt<T>();
    }
    template <typename T>
        requires std::same_as<T, local_id> || std::same_as<T, ip::index>
    [[nodiscard]] constexpr auto as() const noexcept -> T {
        return data.template as<T>();
    }

    [[nodiscard]] constexpr auto is_local() const noexcept -> bool { return data.is<local_id>(); }
    [[nodiscard]] constexpr auto is_constant() const noexcept -> bool {
        return data.is<ip::index>();
    }
    [[nodiscard]] constexpr auto local() const noexcept -> local_id { return data.as<local_id>(); }
    [[nodiscard]] constexpr auto constant_index() const noexcept -> ip::index {
        return data.as<ip::index>();
    }

    // The constant's payload when it is a `T`, none for a local or another kind of constant
    template <typename T>
    [[nodiscard]] auto literal_opt(const ip::pool& pool) const -> stdx::option<const T&> {
        if (!is_constant() || pool.is_type(constant_index())) { return stdx::none; }
        return pool.value_at(constant_index()).data.template as_opt<T>();
    }
    template <typename T> [[nodiscard]] auto is_literal(const ip::pool& pool) const -> bool {
        return literal_opt<T>(pool).has_value();
    }
    // The type a `type` constant names, none for anything else
    [[nodiscard]] auto denoted_type_opt(const ip::pool& pool) const -> stdx::option<sema::type&> {
        if (!is_constant() || !pool.is_type(constant_index())) { return stdx::none; }
        return pool.type_at(constant_index());
    }
    // Whether this is a `type` constant, including one that names no type
    [[nodiscard]] auto is_type_constant(const ip::pool& pool) const -> bool {
        return denoted_type_opt(pool) || is_literal<ip::no_type_value>(pool);
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
    bool                          is_saturating{false}; // `+| -| *| <<|` clamp instead of wrap
    bool                          explicit_volatile{false};

    stdx::option<u8>  atomic_op{stdx::none};         // `@atomicRmw`'s op
    stdx::option<u8>  atomic_order{stdx::none};      // `atomic_order` is the memory order
    stdx::option<u8>  atomic_fail_order{stdx::none}; // `@cmpxchgWeak`/`Strong`'s failure order
    stdx::option<u64> alignment{stdx::none};         // an `@[align(n)]` stack slot
    stdx::option<u8>  lane_op{stdx::none};           // `REDUCE`'s `builtin.ReduceOp`
    stdx::option<ast::call_modifier> call_modifier{stdx::none}; // `@call(modifier, ...)`

    // A `@branchHint`ed `COND_GOTO`: relative weights of its true and false targets
    stdx::option<std::pair<u32, u32>> branch_weights{stdx::none};
    bool                              unpredictable{false};

    [[nodiscard]] auto is_terminator() const noexcept -> bool { return gir::is_terminator(kind); }
    [[nodiscard]] auto has_result() const noexcept -> bool { return result.has_value(); }
    [[nodiscard]] auto is_volatile() const noexcept -> bool {
        return explicit_volatile || (type && type->is_volatile()) ||
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
