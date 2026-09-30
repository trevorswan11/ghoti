#pragma once

#include <string>
#include <string_view>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/syntax/token_type.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::gir::semantics {

// A concrete integer type's shape. Values travel as two's-complement bits in the low `bits`.
struct int_domain {
    u16  bits;
    bool is_signed;
};

enum class int_op : u8 {
    ADD,
    SUB,
    MUL,
    DIV,
    REM,
    WRAP_ADD,
    WRAP_SUB,
    WRAP_MUL,
    WRAP_SHL,
    SAT_ADD,
    SAT_SUB,
    SAT_MUL,
    SAT_SHL,
    AND,
    OR,
    XOR,
    SHL,
    SHR,
    MIN,
    MAX,
    DIV_FLOOR,
    MOD,
};

enum class int_unary_op : u8 {
    NEG,
    WRAP_NEG,
    NOT,
    ABS,
    CLZ,
    CTZ,
    POPCOUNT,
};

enum class int_compare_op : u8 {
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
};

// The integer operation an operator or builtin token denotes; `@divTrunc` = `/` `@rem` = `%`
[[nodiscard]] auto int_op_of(syntax::token_type_t token) noexcept -> stdx::option<int_op>;
[[nodiscard]] auto int_compare_op_of(syntax::token_type_t token) noexcept
    -> stdx::option<int_compare_op>;
// The integer operation a GIR arithmetic instruction performs
[[nodiscard]] auto int_op_of(instruction_kind kind, bool wrapping, bool saturating) noexcept
    -> stdx::option<int_op>;

// Why an operation has no result for these operands. Folding reports it; runtime safety panics.
enum class int_fault : u8 {
    OVERFLOW_,
    DIVISION_BY_ZERO,
    MODULO_BY_ZERO,
    NEGATIVE_SHIFT,
    SHIFT_TOO_WIDE,
    OUT_OF_RANGE,
};

template <typename T> using int_result = stdx::result<T, int_fault>;

// The compile-time diagnostic for a fault; `OUT_OF_RANGE` is phrased by the cast that raised it
[[nodiscard]] auto describe(int_fault fault, int_domain domain, std::string_view type_name)
    -> std::string;

// Sign- or zero-extends a domain's bits to the exact value
[[nodiscard]] auto signed_value(u128 bits, int_domain domain) noexcept -> i128;
[[nodiscard]] auto unsigned_value(u128 bits, int_domain domain) noexcept -> u128;
// The low `domain.bits` of a value's two's-complement pattern
[[nodiscard]] auto to_domain(u128 bits, int_domain domain) noexcept -> u128;

// `lhs op rhs` in `domain`; a shift's amount is `rhs` read in the same domain
[[nodiscard]] auto fold_int(int_op op, int_domain domain, u128 lhs, u128 rhs) -> int_result<u128>;
// A shift (`SHL`, `SHR`, `WRAP_SHL`, `SAT_SHL`) of `lhs` by an amount of any integer type
[[nodiscard]] auto fold_int_shift(int_op op, int_domain domain, u128 lhs, i128 amount)
    -> int_result<u128>;
[[nodiscard]] auto is_shift(int_op op) noexcept -> bool;
[[nodiscard]] auto fold_int_unary(int_unary_op op, int_domain domain, u128 value)
    -> int_result<u128>;
[[nodiscard]] auto compare_int(int_compare_op op, int_domain domain, u128 lhs, u128 rhs) noexcept
    -> bool;

// `@intCast`: the same value in `to`, or a fault when it doesn't fit
[[nodiscard]] auto cast_int(int_domain from, int_domain to, u128 value) -> int_result<u128>;
// `@truncate` and `@bitCast`: the low bits, reinterpreted
[[nodiscard]] auto truncate_int(int_domain to, u128 value) noexcept -> u128;

// `@floatFromInt`: the integer rounded once into `format`; a magnitude past the format's range is
// an infinity, as the conversion instruction produces
[[nodiscard]] auto float_from_int(int_domain from, u128 value, float_format format) -> f128;
// `@intFromFloat`: truncated toward zero, or `OUT_OF_RANGE` for NaN, an infinity, or a value `to`
// can't hold
[[nodiscard]] auto int_from_float(f128 value, int_domain to) -> int_result<u128>;

// What generated code must check under runtime safety for an operation
struct int_checks {
    bool overflow{false};     // the exact result must fit the domain
    bool division{false};     // a zero divisor, and `MIN / -1` when signed
    bool shift_amount{false}; // the amount must be below the width
};

[[nodiscard]] auto runtime_checks(int_op op, int_domain domain) noexcept -> int_checks;
[[nodiscard]] auto runtime_checks(int_unary_op op, int_domain domain) noexcept -> int_checks;

[[nodiscard]] constexpr auto any(int_checks checks) noexcept -> bool {
    return checks.overflow || checks.division || checks.shift_amount;
}

enum class float_op : u8 {
    ADD,
    SUB,
    MUL,
    DIV,
    REM,
    MIN,
    MAX,
};

[[nodiscard]] auto float_op_of(syntax::token_type_t token) noexcept -> stdx::option<float_op>;

// IEEE 754 in `format`, rounded once. Floats never fault: `x / 0` is an infinity or NaN, and
// `@min`/`@max` are minimumNumber/maximumNumber (a NaN operand loses, and -0 < +0).
[[nodiscard]] auto fold_float(float_op op, float_format format, f128 lhs, f128 rhs) -> f128;

} // namespace ghoti::gir::semantics
