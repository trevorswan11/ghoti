#include "compiler/ast/primitive.hh"

#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

#include <stdx/assert.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

namespace {

// A global buffer for storing underscore-cleaned numeric tokens for `std::from_chars`
constinit stdx::fixed::vector<char, 1'024> numeric_buffer;

// Parses the requested value from the string, asserting the from_chars result if requested
template <typename ValueType>
[[nodiscard]] auto parse_primitive_value(std::string_view slice, syntax::token_type_t type) noexcept
    -> stdx::option<ValueType> {
    const auto base{syntax::token_type::to_base(type)};
    {
        // This is narrowly scoped to allow the first and last pointer names to be reused
        const auto* first =
            slice.cbegin() + (!base || *base == syntax::numeric_base::DECIMAL ? 0 : 2);
        const auto* last = slice.cend() - syntax::token_type::suffix_length(type);

        // Strip out the underscores from the slice
        VERIFY(static_cast<usize>(last - first) < numeric_buffer.capacity(), "Literal too long");
        numeric_buffer.clear();
        for (const auto* ptr = first; ptr != last; ++ptr) {
            if (*ptr != '_') { numeric_buffer.emplace_back(*ptr); }
        }
    }

    // The fixed buffer's end pointer is based on the current size, not underlying capacity
    const auto* first = numeric_buffer.begin();
    const auto* last  = numeric_buffer.end();

    ValueType              v;
    std::from_chars_result result;
    if constexpr (std::floating_point<ValueType>) {
        result = std::from_chars(first, last, v);
    } else {
        result = std::from_chars(first, last, v, std::to_underlying(*base));
    }

    // There should only be one error case, hence the use of option vs. result
    if (result.ec == std::errc{} && result.ptr == last) { return v; }
    ASSERT(result.ec == std::errc::result_out_of_range);
    return stdx::none;
}

template <ValuedPrimitive Primitive>
auto parse_primitive(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    using value_type = typename Primitive::value_type;
    const auto start_token{parser.get_current_token()};
    const auto value{parse_primitive_value<value_type>(start_token.slice, start_token.type)};
    if (value) { return parser.add_expr<Primitive>(start_token, *value); }

    syntax::error error_code;
    if constexpr (std::is_same_v<value_type, f64>) {
        error_code = syntax::error::DOUBLE_OVERFLOW;
    } else if constexpr (std::is_same_v<value_type, f32>) {
        error_code = syntax::error::FLOAT_OVERFLOW;
    } else {
        error_code = syntax::error::INTEGER_OVERFLOW;
    }
    return make_syntax_err("Overflow of literal", error_code, start_token);
}

} // namespace

#define MAKE_PRIMITIVE_PARSER(Type)                                                             \
    auto Type::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> { \
        PROFILE_FUNCTION();                                                                     \
        return parse_primitive<Type>(parser);                                                   \
    }

MAKE_PRIMITIVE_PARSER(i32_expr)
MAKE_PRIMITIVE_PARSER(i64_expr)
MAKE_PRIMITIVE_PARSER(isize_expr)
MAKE_PRIMITIVE_PARSER(u32_expr)
MAKE_PRIMITIVE_PARSER(u64_expr)
MAKE_PRIMITIVE_PARSER(usize_expr)

auto u8_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    const auto slice{start_token.slice};
    if (slice[1] != '\\') {
        return parser.add_expr<u8_expr>(start_token, static_cast<u8>(slice[1]));
    }

    const auto escaped{slice[2]};
    u8         value;
    switch (escaped) {
    case 'n':  value = '\n'; break;
    case 'r':  value = '\r'; break;
    case 't':  value = '\t'; break;
    case '\\': value = '\\'; break;
    case '\'': value = '\''; break;
    case '0':  value = '\0'; break;
    default:   return make_syntax_err(syntax::error::UNKNOWN_CHARACTER_ESCAPE, start_token);
    }

    return parser.add_expr<u8_expr>(start_token, value);
}

MAKE_PRIMITIVE_PARSER(f32_expr)
MAKE_PRIMITIVE_PARSER(f64_expr)

auto bool_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    return parser.add_expr<bool_expr>(parser.get_current_token());
}

auto void_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<void_expr>(start_token);
}

auto undefined_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    return parser.add_expr<undefined_expr>(parser.get_current_token());
}

} // namespace ghoti::ast
