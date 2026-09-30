#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/float128.hh"

namespace ghoti {

enum class math_function : u8 {
    SQRT,
    SIN,
    COS,
    TAN,
    EXP,
    EXP2,
    LOG,
    LOG2,
    LOG10,
    FLOOR,
    CEIL,
};

[[nodiscard]] auto math_function_name(math_function function) noexcept -> std::string_view;

// Working precision never passes this; no known input comes close
constexpr u32 MAX_MATH_WORKING_BITS{1U << 15};

struct math_result {
    f128 value;
    // The precision the result was decided at; zero when no approximation was needed
    u32 working_bits{0};
    // A finite input whose finite result is past the format's range
    bool overflowed{false};
};

// Where an untyped result came from, so rounding it into a narrower format later can recompute
// it there instead of rounding twice
struct math_origin {
    math_function function;
    f128          input;

    [[nodiscard]] auto operator==(const math_origin& other) const noexcept -> bool {
        return function == other.function && input.bits() == other.input.bits();
    }
};

// `function(x)` correctly rounded into `format`: the infinitely precise result, rounded once to
// nearest with ties to even. Special values follow IEEE 754.
[[nodiscard]] auto evaluate_traced(math_function function, f128 x, float_format format)
    -> math_result;
[[nodiscard]] auto
evaluate(math_function function, f128 x, float_format format = float_format::QUAD) -> f128;

} // namespace ghoti
