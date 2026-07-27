#include "compiler/syntax/lexer.hh"

#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/operators.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

auto lexer::reset(std::string_view input) noexcept -> void { *this = lexer{input}; }

auto lexer::advance() noexcept -> token_t {
    PROFILE_FUNCTION();
    skip_whitespace();

    token_t    token{{}, {}, line_no_, col_no_};
    const auto maybe_operator{read_operator()};

    if (maybe_operator) {
        if (maybe_operator->type == token_type_t::END) { return *maybe_operator; }
        for (usize i{0}; i < maybe_operator->slice.size(); ++i) { read_character(); }

        if (maybe_operator->type == token_type_t::COMMENT) { return read_comment(); }
        if (maybe_operator->type == token_type_t::MULTILINE_STRING) {
            return read_multiline_string();
        }

        return *maybe_operator;
    }

    const auto maybe_misc_token_type{token_type::misc_from_char(current_byte_)};
    if (maybe_misc_token_type) {
        token.slice = stdx::string::substr(input_, pos_, 1);
        token.type  = *maybe_misc_token_type;
    } else if (current_byte_ == '@') {
        token.slice = read_ident(true);
        token.type  = lu_builtin(token.slice);
        return token;
    } else if (std::isalpha(current_byte_)) {
        token.slice = read_ident(false);
        token.type  = lu_ident(token.slice);
        return token;
    } else if (std::isdigit(current_byte_)) {
        return read_number();
    } else if (current_byte_ == '"') {
        return read_string();
    } else if (current_byte_ == '\'') {
        return read_byte_literal();
    } else {
        token.slice = stdx::string::substr(input_, pos_, 1);
        token.type  = token_type_t::ILLEGAL;
    }

    read_character();
    return token;
}

auto lexer::consume() -> std::vector<token_t> {
    reset(input_);

    std::vector<token_t> tokens;
    do { tokens.emplace_back(advance()); } while (tokens.back().type != token_type_t::END);

    return tokens;
}

auto lexer::skip_whitespace() noexcept -> void {
    while (std::isspace(current_byte_)) { read_character(); }
}

auto lexer::lu_builtin(std::string_view ident) noexcept -> token_type_t {
    return get_builtin_opt(ident).value_or(token_type_t::ILLEGAL);
}

auto lexer::lu_ident(std::string_view ident) noexcept -> token_type_t {
    return get_keyword_opt(ident).value_or(token_type_t::IDENT);
}

auto lexer::read_character(u8 n) noexcept -> void {
    for (u8 i = 0; i < n; ++i) {
        if (current_byte_ == '\n') {
            line_no_ += 1;
            col_no_ = 0;
        } else if (current_byte_ == '\r' &&
                   (peek_pos_ >= input_.size() || input_[peek_pos_] != '\n')) {
            line_no_ += 1;
            col_no_ = 0;
        } else if (pos_ != 0 || peek_pos_ != 0) {
            col_no_ += 1;
        }

        if (peek_pos_ >= input_.size()) {
            current_byte_ = '\0';
        } else {
            current_byte_ = input_[peek_pos_];
        }

        pos_ = peek_pos_;
        peek_pos_ += 1;
    }
}

auto lexer::read_operator() const noexcept -> stdx::option<token_t> {
    const auto start_line{line_no_};
    const auto start_col{col_no_};

    if (current_byte_ == '\0') { return token_t{token_type_t::END, {}, start_line, start_col}; }

    usize max_len{0};
    auto  matched_type{token_type_t::ILLEGAL};

    // Try extending from length 1 up to the max operator size
    for (usize len{1}; len <= max_operator_length() && pos_ + len <= input_.size(); ++len) {
        if (const auto op{get_operator_opt(stdx::string::substr(input_, pos_, len))}) {
            matched_type = *op;
            max_len      = len;
        }
    }

    // We cannot greedily consume the lexer here since the next token instruction handles that
    if (max_len == 0) { return stdx::none; }
    return token_t{
        matched_type, stdx::string::substr(input_, pos_, max_len), start_line, start_col};
}

auto lexer::read_ident(bool builtin) noexcept -> std::string_view {
    const auto start{pos_};

    auto passed_first{false};
    while ((builtin && !passed_first && current_byte_ == '@') || std::isalpha(current_byte_) ||
           current_byte_ == '_' || (passed_first && std::isdigit(current_byte_))) {
        read_character();
        passed_first = true;
    }

    return stdx::string::substr(input_, start, pos_ - start);
}

enum class number_suffix : u8 {
    UNSIGNED = 1 << 0,
    WIDE     = 1 << 1,
    SIZE     = 2 << 2,
};

namespace {

constexpr auto operator|=(number_suffix& lhs, number_suffix rhs) noexcept -> number_suffix& {
    lhs = static_cast<number_suffix>(std::to_underlying(lhs) | std::to_underlying(rhs));
    return lhs;
}

constexpr auto operator&(number_suffix lhs, number_suffix rhs) noexcept -> number_suffix {
    return static_cast<number_suffix>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

constexpr auto suffix_has(number_suffix suffix, number_suffix flag) noexcept -> bool {
    return static_cast<bool>(suffix & flag);
}

} // namespace

auto lexer::read_number() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    auto       passed_decimal{false};
    auto       passed_exponent{false};
    auto       base{numeric_base::DECIMAL};

    // Detect numeric prefix
    if (current_byte_ == '0' && peek_pos_ < input_.size()) {
        const auto next{input_[peek_pos_]};
        if (next == 'x' || next == 'X') {
            base = numeric_base::HEXADECIMAL;
            read_character(2);
        } else if (next == 'b' || next == 'B') {
            base = numeric_base::BINARY;
            read_character(2);
        } else if (next == 'o' || next == 'O') {
            base = numeric_base::OCTAL;
            read_character(2);
        }
    }

    // Consume digits and handle dot/range rules
    auto last_was_digit{false};
    while (true) {
        const auto c{current_byte_};

        // Exponent handling defaults to floats for simplicity
        if (base == numeric_base::DECIMAL && !passed_exponent && (c == 'e' || c == 'E')) {
            auto p{peek_pos_};
            if (p >= input_.size()) { break; }

            auto next{input_[p]};
            if (next == '+' || next == '-') {
                p += 1;
                if (p >= input_.size()) { break; }
                next = input_[p];
            }

            if (!std::isdigit(next)) { break; }

            passed_exponent = true;
            read_character();

            if (current_byte_ == '+' || current_byte_ == '-') { read_character(); }
            while (std::isdigit(current_byte_)) { read_character(); }
            last_was_digit = true;
            continue;
        }

        // Fractional part
        if (base == numeric_base::DECIMAL && c == '.') {
            if (peek_pos_ < input_.size() && input_[peek_pos_] == '.') { break; }
            if (passed_decimal) { break; }

            passed_decimal = true;
            last_was_digit = false;
            read_character();
            continue;
        }

        // Underscore can only be in between digits
        if (c == '_' && last_was_digit) {
            read_character();
            if (!digit_in_base(current_byte_, base)) {
                return {token_type_t::ILLEGAL,
                        stdx::string::substr(input_, start, pos_ - start),
                        start_line,
                        start_col};
            }
            last_was_digit = false;
            continue;
        }

        // Normal digit
        if (digit_in_base(c, base)) {
            last_was_digit = true;
            read_character();
            continue;
        }

        break;
    }

    // Quick non-base-10 length validation
    if (base != numeric_base::DECIMAL && pos_ - start <= 2) {
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }

    number_suffix suffix{};
    auto          forced_float{false};
    if (pos_ < input_.size()) {
        auto c{current_byte_};
        if (c == 'f' || c == 'F') {
            forced_float = true;
            read_character();
        } else if (!(passed_decimal || passed_exponent)) {
            if (c == 'u' || c == 'U') {
                suffix |= number_suffix::UNSIGNED;
                read_character();
            }

            c = current_byte_;
            if (c == 'z' || c == 'Z') {
                suffix |= number_suffix::SIZE;
                read_character();
            } else if (c == 'l' || c == 'L') {
                suffix |= number_suffix::WIDE;
                read_character();
            }
        }
    }

    // Total validation
    const auto length{pos_ - start};
    auto       type{token_type_t::ILLEGAL};
    if (length == 0) {
        return {type, stdx::string::substr(input_, start, 1), start_line, start_col};
    }

    if (input_[pos_ - 1] == '.') {
        return {type, stdx::string::substr(input_, start, length), start_line, start_col};
    }

    if (passed_decimal && (base != numeric_base::DECIMAL)) {
        return {type, stdx::string::substr(input_, start, length), start_line, start_col};
    }

    // Determine the input type
    if (passed_decimal || passed_exponent || forced_float) {
        if (base != numeric_base::DECIMAL) {
            return {type, stdx::string::substr(input_, start, length), start_line, start_col};
        }
        type = forced_float ? token_type_t::F32 : token_type_t::F64;
    } else {
        // Use an offset to increment the actual token type based on its base and width
        auto offset{base_idx(base)};
        if (std::to_underlying(suffix) == 0) {
            type = token_type_t::INT_2;
        } else {
            if (suffix_has(suffix, number_suffix::WIDE)) {
                type = token_type_t::LINT_2;
            } else if (suffix_has(suffix, number_suffix::SIZE)) {
                type = token_type_t::ZINT_2;
            } else {
                type = token_type_t::INT_2;
            }

            // We can just bump the offset for unsigned
            if (suffix_has(suffix, number_suffix::UNSIGNED)) {
                offset += std::to_underlying(token_type_t::UINT_2) -
                          std::to_underlying(token_type_t::INT_2);
            }
        }
        type = static_cast<token_type_t>(std::to_underlying(type) + offset);
    }

    return {type, stdx::string::substr(input_, start, length), start_line, start_col};
}

auto lexer::read_escape() noexcept -> char {
    read_character();

    switch (current_byte_) {
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    case '0':  return '\0';
    default:   return current_byte_;
    }
}

auto lexer::read_string() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    read_character();

    while (current_byte_ != '"' && current_byte_ != '\0') {
        if (current_byte_ == '\\') { read_escape(); }
        read_character();
    }

    if (current_byte_ == '\0') {
        return {
            token_type_t::ILLEGAL,
            stdx::string::substr(input_, start, pos_ - start),
            start_line,
            start_col,
        };
    }
    read_character();

    return {token_type_t::STRING,
            stdx::string::substr(input_, start, pos_ - start),
            start_line,
            start_col};
}

// Reads a multiline string from the token, assuming the '\\' operator has been consumed
auto lexer::read_multiline_string() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    auto       end_pos{start};

    while (true) {
        // Consume characters until newline or EOF
        while (current_byte_ != '\n' && current_byte_ != '\r' && current_byte_ != '\0') {
            read_character();
        }

        // Peek positions
        usize peek_pos{peek_pos_};
        if (current_byte_ == '\r' && peek_pos < input_.size() && input_[peek_pos] == '\n') {
            peek_pos += 1;
        }

        auto has_continuation{false};
        if ((current_byte_ == '\n' || current_byte_ == '\r') && peek_pos + 1 < input_.size() &&
            input_[peek_pos] == '\\' && input_[peek_pos + 1] == '\\') {
            has_continuation = true;
        }

        // Don't include the newline if there is no continuation to prevent trailing whitespace
        if (!has_continuation) {
            end_pos = pos_;
            break;
        }

        // Include the CRLF/LF newline in the token
        read_character();
        if (current_byte_ == '\r' && peek_pos_ < input_.size() && input_[peek_pos_] == '\n') {
            read_character();
        }

        // consume the next "\\" line continuation
        read_character(2);
    }

    return {
        token_type_t::MULTILINE_STRING,
        stdx::string::substr(input_, start, end_pos - start),
        start_line,
        start_col,
    };
}

// Reads a byte literal returning an illegal token for malformed literals.
//
// Assumes that the surrounding single quotes have not been consumed.
auto lexer::read_byte_literal() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    read_character();

    // Consume one logical character
    if (current_byte_ == '\\') {
        read_escape();
        read_character();
    } else if (current_byte_ != '\'' && current_byte_ != '\n' && current_byte_ != '\r') {
        read_character();
    } else {
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }

    // The next character MUST be closing ', otherwise illegally consume like a comment
    if (current_byte_ != '\'') {
        auto illegal_end{pos_};
        while (current_byte_ != '\'' && current_byte_ != '\n' && current_byte_ != '\r' &&
               current_byte_ != '\0') {
            read_character();
            illegal_end = pos_;
        }

        if (current_byte_ == '\'') {
            read_character();
            illegal_end = pos_;
        }

        return {
            token_type_t::ILLEGAL,
            stdx::string::substr(input_, start, illegal_end - start),
            start_line,
            start_col,
        };
    }
    read_character();

    return {
        token_type_t::U8, stdx::string::substr(input_, start, pos_ - start), start_line, start_col};
}

// Reads a comment from the token, assuming the '//' operator has been consumed
auto lexer::read_comment() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    while (current_byte_ != '\n' && current_byte_ != '\0') { read_character(); }

    return {token_type_t::COMMENT,
            stdx::string::substr(input_, start, pos_ - start),
            start_line,
            start_col};
}

} // namespace ghoti::syntax
