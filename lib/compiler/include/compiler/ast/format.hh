#pragma once

// IWYU pragma: begin_exports

#include <fmt/base.h>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"

template <> struct fmt::formatter<ghoti::ast::type_modifier> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::ast::type_modifier& t, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", magic_enum::enum_name(t.underlying_));
    }
};

template <> struct fmt::formatter<ghoti::ast::identifier_expr> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::ast::identifier_expr& n, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", n.name);
    }
};

template <ghoti::ast::ValuedPrimitive Primitive> struct fmt::formatter<Primitive> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const Primitive& p, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", p.value);
    }
};

// IWYU pragma: end_exports
