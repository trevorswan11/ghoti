#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

enum class bind_precedence : u8 {
    LOWEST           = 0,
    ASSIGNMENT       = 10,
    RANGE            = 15,
    BOOL_AND_OR      = 20,
    BOOL_EQUIV       = 30,
    BOOL_LT_GT       = 40,
    ADD_SUB          = 50,
    MUL_DIV          = 60,
    EXPONENT         = 70,
    PREFIX           = 80,
    INITIALIZATION   = 100,
    TYPE             = 110,
    SCOPE_RESOLUTION = 120,
    GROUP_CALL_IDX   = 130,
    LABEL            = 140,
};

struct binding {
    bind_precedence precedence;
    bool            right_assoc{false};

    [[nodiscard]] static auto try_get_from(token_type_t tt) noexcept -> stdx::option<binding>;
};

} // namespace ghoti::syntax
