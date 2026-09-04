#include "compiler/ast/primitive.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <stdx/assert.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token_type.hh"
#include "stdx/string.hh"
#include "support/int128.hh"
#include "support/string_utils.hh"

namespace ghoti::ast {

namespace {

// `std::from_chars` has no 128-bit overload; fold digits by hand and flag overflow.
[[nodiscard]] auto parse_u128(std::string_view digits, u32 radix, bool& overflow) noexcept -> u128 {
    constexpr u128 max{~static_cast<u128>(0)};
    u128           value{0};
    for (const char c : digits) {
        if (c == '_') { continue; }
        u32 d{};
        if (c >= '0' && c <= '9') {
            d = static_cast<u32>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<u32>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<u32>(c - 'A') + 10;
        } else {
            overflow = true; // not a digit in any supported base
            return value;
        }
        if (value > (max - d) / radix) {
            overflow = true;
            return value;
        }
        value = value * radix + d;
    }
    return value;
}

[[nodiscard]] auto all_digits(std::string_view s) noexcept -> bool {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

[[nodiscard]] auto parse_width(std::string_view s) noexcept -> u32 {
    u64 w{0};
    for (const char c : s) {
        w = w * 10 + static_cast<u64>(c - '0');
        if (w > 0xFFFF) { return 0; }
    }
    return static_cast<u32>(w);
}

struct int_suffix {
    u16  width{0};
    bool is_signed{false};
    bool is_size{false};
};

[[nodiscard]] auto classify_int_suffix(std::string_view suffix) noexcept
    -> stdx::result<int_suffix, std::string> {
    if (suffix.empty()) { return int_suffix{}; }
    if (suffix == "z" || suffix == "Z") { return int_suffix{.is_signed = true, .is_size = true}; }
    if (suffix == "uz" || suffix == "uZ" || suffix == "Uz" || suffix == "UZ") {
        return int_suffix{.is_signed = false, .is_size = true};
    }

    const char head{suffix.front()};
    const auto rest{suffix.substr(1)};
    if ((head == 'u' || head == 'i') && all_digits(rest) && rest.front() != '0') {
        const auto width{parse_width(rest)};
        if (width == 0) {
            return stdx::make_err<std::string>("integer literal width must be 1..65535");
        }
        return int_suffix{
            .width = static_cast<u16>(width), .is_signed = head == 'i', .is_size = false};
    }

    if (head == 'l' || head == 'L' || suffix == "ul" || suffix == "uL") {
        return stdx::make_err<std::string>(
            "the 'l'/'L' integer literal suffix is not supported; use an explicit width "
            "like '42i64'");
    } else if (head == 'u' || head == 'i') {
        return stdx::make_err<std::string>("integer literal suffix needs a width, e.g. 42u8");
    } else {
        return stdx::make_err<std::string>("unknown integer literal suffix");
    }
}

// Splits a lexeme into `<prefix><digits>` and a trailing suffix
[[nodiscard]] auto split_int_lexeme(std::string_view lexeme, syntax::numeric_base base)
    -> std::pair<std::string_view, std::string_view> {
    const usize prefix{base == syntax::numeric_base::DECIMAL ? 0UZ : 2UZ};
    const auto  ends_with = [prefix, &lexeme](std::string_view suf) {
        return lexeme.size() >= prefix + suf.size() &&
               string_utils::ends_with_ci(stdx::string::substr(lexeme, lexeme.size() - suf.size()),
                                          suf);
    };

    if (ends_with("uz")) {
        return {lexeme.substr(0, lexeme.size() - 2), lexeme.substr(lexeme.size() - 2)};
    } else if (ends_with("z")) {
        return {lexeme.substr(0, lexeme.size() - 1), lexeme.substr(lexeme.size() - 1)};
    }

    // `u<W>` / `i<W>`: a trailing decimal run preceded by `u`/`i`.
    usize k{lexeme.size()};
    while (k > prefix && lexeme[k - 1] >= '0' && lexeme[k - 1] <= '9') { --k; }
    if (k < lexeme.size() && k > prefix) {
        const char c{static_cast<char>(std::tolower(lexeme[k - 1]))};
        if (c == 'u' || c == 'i') { return {lexeme.substr(0, k - 1), lexeme.substr(k - 1)}; }
        return {lexeme, {}}; // trailing digits belong to the number
    }

    usize j{lexeme.size()};
    while (j > prefix && std::isalpha(static_cast<u8>(lexeme[j - 1])) &&
           !syntax::digit_in_base(lexeme[j - 1], base)) {
        --j;
    }
    if (j < lexeme.size() && j >= prefix) { return {lexeme.substr(0, j), lexeme.substr(j)}; }
    return {lexeme, {}};
}

} // namespace

auto int_literal_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    const auto slice{start_token.slice};

    // A byte literal `'x'` is a width-8 unsigned integer.
    if (start_token.type == syntax::token_type_t::U8) {
        u8 value{static_cast<u8>(slice[1])};
        if (slice[1] == '\\') {
            switch (slice[2]) {
            case 'n':  value = '\n'; break;
            case 'r':  value = '\r'; break;
            case 't':  value = '\t'; break;
            case '\\': value = '\\'; break;
            case '\'': value = '\''; break;
            case '"':  value = '"'; break;
            case '0':  value = '\0'; break;
            default:   return make_syntax_err(syntax::error::UNKNOWN_CHARACTER_ESCAPE, start_token);
            }
        }
        return parser.add_expr<int_literal_expr>(start_token,
                                                 int_literal_expr{
                                                     .value     = value,
                                                     .width     = 8,
                                                     .is_signed = false,
                                                     .is_size   = false,
                                                     .base      = syntax::numeric_base::DECIMAL,
                                                     .spelling  = slice,
                                                 });
    }

    const auto base{
        syntax::token_type::to_base(start_token.type).value_or(syntax::numeric_base::DECIMAL)};
    const auto [digits, suffix]{split_int_lexeme(slice, base)};

    const auto info{classify_int_suffix(suffix)};
    if (!info) {
        return make_syntax_err(
            std::move(info.error()), syntax::error::INVALID_NUMBER_LITERAL, start_token);
    }

    const auto digits_only{base == syntax::numeric_base::DECIMAL ? digits
                                                                 : stdx::string::substr(digits, 2)};
    bool       overflow{false};
    const u128 value{parse_u128(digits_only, std::to_underlying(base), overflow)};
    if (overflow) {
        return make_syntax_err("Overflow of literal", syntax::error::INTEGER_OVERFLOW, start_token);
    }

    return parser.add_expr<int_literal_expr>(start_token,
                                             int_literal_expr{
                                                 .value     = value,
                                                 .width     = info->width,
                                                 .is_signed = info->is_signed,
                                                 .is_size   = info->is_size,
                                                 .base      = base,
                                                 .spelling  = slice,
                                             });
}

auto float_literal_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    const auto slice{start_token.slice};

    // Trailing `f<width>` suffix, if any.
    std::string_view mantissa{slice};
    u8               width{0};
    if (const auto pos{slice.find_last_of("fF")}; pos != std::string_view::npos) {
        const auto ws{slice.substr(pos + 1)};
        if (ws.empty() || !all_digits(ws)) {
            return make_syntax_err(
                "float literal suffix needs a width, e.g. 1.0f32; valid widths are 16/32/64/80/128",
                syntax::error::FLOAT_OVERFLOW,
                start_token);
        }
        static constexpr std::array<std::string_view, 5> valid{"16", "32", "64", "80", "128"};
        if (std::ranges::find(valid, ws) == valid.end()) {
            return make_syntax_err("invalid float literal width; valid widths are 16/32/64/80/128",
                                   syntax::error::FLOAT_OVERFLOW,
                                   start_token);
        }
        width    = static_cast<u8>(parse_width(ws));
        mantissa = slice.substr(0, pos);
    }

    using namespace stdx::size_literals;
    static thread_local stdx::fixed::vector<char, 1_KiB> buffer;
    buffer.clear();
    for (const char c : mantissa) {
        if (c != '_') { buffer.emplace_back(c); }
    }

    f64                          value{};
    const std::from_chars_result result{std::from_chars(buffer.begin(), buffer.end(), value)};
    if (result.ec != std::errc{} || result.ptr != buffer.end()) {
        return make_syntax_err("Overflow of literal", syntax::error::DOUBLE_OVERFLOW, start_token);
    }

    return parser.add_expr<float_literal_expr>(
        start_token, float_literal_expr{.value = value, .width = width, .spelling = slice});
}

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

auto nullptr_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    return parser.add_expr<nullptr_expr>(parser.get_current_token());
}

auto unreachable_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    return parser.add_expr<unreachable_expr>(parser.get_current_token());
}

} // namespace ghoti::ast
