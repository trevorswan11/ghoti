#include "compiler/syntax/lexer.hh"

#include <cctype>
#include <string_view>
#include <utility>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/operators.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "compiler/syntax/trvia.hh"

namespace ghoti::syntax {

namespace {

// Re-tags a bare `//` comment token as `///` doc / `//!` module doc, trimming the extra
// marker char and one optional leading space from the slice.
auto classify_comment(token_t token) noexcept -> token_t {
    auto rest{token.slice};
    if (rest.starts_with('!')) {
        token.type = token_type_t::MODULE_DOC_COMMENT;
    } else if (rest.starts_with('/') && !rest.starts_with("//")) {
        token.type = token_type_t::DOC_COMMENT;
    } else {
        return token;
    }
    rest.remove_prefix(1);
    if (rest.starts_with(' ')) { rest.remove_prefix(1); }
    token.slice = rest;
    return token;
}

// The <cctype> classifiers are undefined for negative `char` values (any non-ASCII byte)
[[nodiscard]] auto is_alpha(char c) noexcept -> bool { return std::isalpha(static_cast<u8>(c)); }
[[nodiscard]] auto is_digit(char c) noexcept -> bool { return std::isdigit(static_cast<u8>(c)); }
[[nodiscard]] auto is_alnum(char c) noexcept -> bool { return std::isalnum(static_cast<u8>(c)); }
[[nodiscard]] auto is_space(char c) noexcept -> bool { return std::isspace(static_cast<u8>(c)); }

} // namespace

auto lexer::reset(std::string_view input) noexcept -> void { *this = lexer{input}; }

auto lexer::advance() noexcept -> token_t {
    PROFILE_FUNCTION();
    skip_whitespace();

    token_t    token{{}, {}, line_no_, col_no_};
    const auto maybe_operator{read_operator()};

    if (maybe_operator) {
        if (maybe_operator->type == token_type_t::END) { return *maybe_operator; }
        for (usize i{0}; i < maybe_operator->slice.size(); ++i) { read_character(); }

        if (maybe_operator->type == token_type_t::COMMENT) {
            return classify_comment(read_comment());
        }
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
        if (peek_pos_ < input_.size() && input_[peek_pos_] == '"') { return read_raw_identifier(); }
        token.slice = read_ident(true);
        token.type  = lu_builtin(token.slice);
        return token;
    } else if (is_alpha(current_byte_) || current_byte_ == '_') {
        if (current_byte_ == '_') {
            const auto next_c{peek_pos_ < input_.size() ? input_[peek_pos_] : '\0'};
            if (!is_alnum(next_c) && next_c != '_') {
                token.slice = stdx::string::substr(input_, pos_, 1);
                token.type  = token_type_t::UNDERSCORE;
                read_character();
                return token;
            }
        }
        token.slice = read_ident(false);
        token.type  = lu_ident(token.slice);
        return token;
    } else if (is_digit(current_byte_)) {
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

auto lexer::advance_enriched() noexcept -> enriched_token {
    enriched_token result;

    // Collect leading trivia
    while (pos_ < input_.size() && (is_space(current_byte_) || current_byte_ == '/')) {
        if (current_byte_ == '/' && peek_pos_ < input_.size() && input_[peek_pos_] != '/') {
            break;
        }

        const auto start_line{line_no_};
        const auto start_col{col_no_};
        const auto start_pos{pos_};

        if (current_byte_ == '\n') {
            read_character();
            result.leading_trivia.emplace_back(trivia_kind::NEWLINE, "\n", start_line, start_col);
        } else if (is_space(current_byte_)) {
            while (pos_ < input_.size() && is_space(current_byte_) && current_byte_ != '\n') {
                read_character();
            }
            result.leading_trivia.emplace_back(
                trivia_kind::WHITESPACE,
                stdx::string::substr(input_, start_pos, pos_ - start_pos),
                start_line,
                start_col);
        } else if (current_byte_ == '/') {
            const auto comment{read_comment()};
            result.leading_trivia.emplace_back(
                trivia_kind::LINE_COMMENT, comment.slice, comment.line, comment.column);
        }
    }

    // Lex the actual token
    result.token = advance();
    if (result.token.type == token_type_t::END) { return result; }

    // Collect trailing same-line trivia
    const auto token_line{result.token.line};
    while (pos_ < input_.size() && line_no_ == token_line && current_byte_ != '\n') {
        if (current_byte_ == ' ' || current_byte_ == '\t') {
            read_character(); // trimmed
            continue;
        }

        // Single line comments consume the rest of the line
        if (current_byte_ == '/' && peek_pos_ < input_.size() && input_[peek_pos_] == '/') {
            const auto comment_line{line_no_};
            const auto comment_col{col_no_};
            const auto comment{read_comment()};
            result.trailing_trivia.emplace_back(
                trivia_kind::LINE_COMMENT, comment.slice, comment_line, comment_col);
        }
        break;
    }

    return result;
}

auto lexer::skip_whitespace() noexcept -> void {
    while (is_space(current_byte_)) { read_character(); }
}

auto lexer::lu_builtin(std::string_view ident) noexcept -> token_type_t {
    return get_builtin_opt(ident).value_or(token_type_t::ILLEGAL);
}

auto lexer::lu_ident(std::string_view ident) noexcept -> token_type_t {
    if (token_type::is_int_type_lexeme(ident)) { return token_type_t::INT_TYPE; }
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

    if (at_end()) { return token_t{token_type_t::END, {}, start_line, start_col}; }

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
    const auto matched_text{stdx::string::substr(input_, pos_, max_len)};

    // A word-operator must be a whole word to allow e.g. origin to work
    if (!matched_text.empty() && (is_alpha(matched_text.front()) || matched_text.front() == '_') &&
        pos_ + max_len < input_.size()) {
        const auto next{input_[pos_ + max_len]};
        if (is_alnum(next) || next == '_') { return stdx::none; }
    }

    return token_t{matched_type, matched_text, start_line, start_col};
}

auto lexer::read_ident(bool builtin) noexcept -> std::string_view {
    const auto start{pos_};

    auto passed_first{false};
    while ((builtin && !passed_first && current_byte_ == '@') || is_alpha(current_byte_) ||
           current_byte_ == '_' || (passed_first && is_digit(current_byte_))) {
        read_character();
        passed_first = true;
    }

    return stdx::string::substr(input_, start, pos_ - start);
}

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

        // Exponent handling defaults to floats for simplicity; hex floats use a binary `p` exponent
        const bool exponent_marker{base == numeric_base::HEXADECIMAL ? (c == 'p' || c == 'P')
                                                                     : (c == 'e' || c == 'E')};
        const bool has_mantissa{base == numeric_base::DECIMAL ||
                                (base == numeric_base::HEXADECIMAL && last_was_digit)};
        if (has_mantissa && !passed_exponent && exponent_marker) {
            auto p{peek_pos_};
            if (p >= input_.size()) { break; }

            auto next{input_[p]};
            if (next == '+' || next == '-') {
                p += 1;
                if (p >= input_.size()) { break; }
                next = input_[p];
            }

            if (!is_digit(next)) { break; }

            passed_exponent = true;
            read_character();

            if (current_byte_ == '+' || current_byte_ == '-') { read_character(); }
            while (is_digit(current_byte_)) { read_character(); }
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

        // A hex fraction needs digits on both sides of the '.', leaving `0x1..2` and `0x1.len`
        // alone
        if (base == numeric_base::HEXADECIMAL && c == '.' && last_was_digit && !passed_decimal &&
            !passed_exponent && peek_pos_ < input_.size() &&
            digit_in_base(input_[peek_pos_], numeric_base::HEXADECIMAL)) {
            passed_decimal = true;
            last_was_digit = false;
            read_character();
            continue;
        }

        // Underscore can only be in between digits; exponent digits are always decimal
        const auto digit_base{passed_exponent ? numeric_base::DECIMAL : base};
        if (c == '_' && last_was_digit) {
            read_character();
            if (!digit_in_base(current_byte_, digit_base)) {
                return {token_type_t::ILLEGAL,
                        stdx::string::substr(input_, start, pos_ - start),
                        start_line,
                        start_col};
            }
            last_was_digit = false;
            continue;
        }

        // Normal digit
        if (digit_in_base(c, digit_base)) {
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

    // Consume for diagnostics, the parser validates the exact grammar
    const auto suffix_start{pos_};
    if (pos_ < input_.size()) {
        switch (current_byte_) {
        case 'u':
        case 'U':
        case 'i':
        case 'I':
        case 'z':
        case 'Z':
        case 'l':
        case 'L':
        case 'f':
        case 'F':
            while (pos_ < input_.size() &&
                   (is_alpha(current_byte_) || (pos_ > suffix_start && is_digit(current_byte_)))) {
                read_character();
            }
            break;
        default: break;
        }
    }
    const auto suffix{stdx::string::substr(input_, suffix_start, pos_ - suffix_start)};

    // Glued trailing characters (`0b102`, `1e`, `12abc`) make the whole literal malformed
    if (!at_end() && (is_alnum(current_byte_) || current_byte_ == '_')) {
        while (!at_end() && (is_alnum(current_byte_) || current_byte_ == '_')) { read_character(); }
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }

    const auto length{pos_ - start};
    const auto lexeme{stdx::string::substr(input_, start, length)};
    if (length == 0) {
        return {
            token_type_t::ILLEGAL, stdx::string::substr(input_, start, 1), start_line, start_col};
    }

    // A trailing bare '.' or a fractional binary/octal literal is malformed.
    const bool float_base{base == numeric_base::DECIMAL || base == numeric_base::HEXADECIMAL};
    if (input_[pos_ - 1] == '.' || (passed_decimal && !float_base)) {
        return {token_type_t::ILLEGAL, lexeme, start_line, start_col};
    }

    const bool has_float_suffix{!suffix.empty() &&
                                (suffix.front() == 'f' || suffix.front() == 'F')};
    if (passed_decimal || passed_exponent || has_float_suffix) {
        if (!float_base) { return {token_type_t::ILLEGAL, lexeme, start_line, start_col}; }
        return {token_type_t::REAL, lexeme, start_line, start_col};
    }

    const auto type{
        static_cast<token_type_t>(std::to_underlying(token_type_t::INT_2) + base_idx(base))};
    return {type, lexeme, start_line, start_col};
}

auto lexer::read_escape() noexcept -> bool {
    read_character();
    return !at_end() && decode_escape(current_byte_).has_value();
}

auto lexer::read_string() noexcept -> token_t {
    const auto start{pos_};
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    read_character();

    auto escapes_valid{true};
    while (current_byte_ != '"' && !at_end()) {
        if (current_byte_ == '\\' && !read_escape()) { escapes_valid = false; }
        read_character();
    }

    // A terminated literal with a bad escape keeps its closing quote so the parser can tell
    if (!at_end() && !escapes_valid) {
        read_character();
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }

    if (at_end()) {
        return {
            token_type_t::ILLEGAL,
            stdx::string::substr(input_, start, pos_ - start),
            start_line,
            start_col,
        };
    }
    read_character();

    const auto lexeme{stdx::string::substr(input_, start, pos_ - start)};
    return {is_valid_utf8(lexeme) ? token_type_t::STRING : token_type_t::ILLEGAL,
            lexeme,
            start_line,
            start_col};
}

auto lexer::read_raw_identifier() noexcept -> token_t {
    const auto start{pos_}; // Leading '@'
    const auto start_line{line_no_};
    const auto start_col{col_no_};
    read_character(2); // consume '@' and the opening '"'

    auto escapes_valid{true};
    while (current_byte_ != '"' && !at_end() && current_byte_ != '\n' && current_byte_ != '\r') {
        if (current_byte_ == '\\' && !read_escape()) { escapes_valid = false; }
        read_character();
    }

    // An unterminated raw identifier surfaces as ILLEGAL
    if (current_byte_ != '"') {
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }
    read_character(); // consume the closing '"'

    if (!escapes_valid) {
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start, pos_ - start),
                start_line,
                start_col};
    }

    // The slice keeps the whole `@"..."` lexeme for the parser to intern and interpret
    return {token_type_t::IDENT,
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
        while (current_byte_ != '\n' && current_byte_ != '\r' && !at_end()) { read_character(); }

        // Peek positions
        usize peek_pos{peek_pos_};
        if (current_byte_ == '\r' && peek_pos < input_.size() && input_[peek_pos] == '\n') {
            peek_pos += 1;
        }

        // Indentation before the continuation marker is allowed
        usize marker_pos{peek_pos};
        while (marker_pos < input_.size() &&
               (input_[marker_pos] == ' ' || input_[marker_pos] == '\t')) {
            marker_pos += 1;
        }

        auto has_continuation{false};
        if ((current_byte_ == '\n' || current_byte_ == '\r') && marker_pos + 1 < input_.size() &&
            input_[marker_pos] == '\\' && input_[marker_pos + 1] == '\\') {
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

        // consume the indentation preceding the "\\\\" line continuation marker
        while (current_byte_ == ' ' || current_byte_ == '\t') { read_character(); }

        // consume the next "\\" line continuation
        read_character(2);
    }

    const auto content{stdx::string::substr(input_, start, end_pos - start)};
    if (!is_valid_utf8(content)) {
        // Keep the opening `\\` marker so the parser can tell which literal was malformed
        return {token_type_t::ILLEGAL,
                stdx::string::substr(input_, start - 2, end_pos - start + 2),
                start_line,
                start_col - 2};
    }
    return {
        token_type_t::MULTILINE_STRING,
        content,
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
    // An unknown escape still lexes; the parser reports it as UNKNOWN_CHARACTER_ESCAPE
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
               !at_end()) {
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
    while (current_byte_ != '\n' && !at_end()) { read_character(); }

    return {token_type_t::COMMENT,
            stdx::string::substr(input_, start, pos_ - start),
            start_line,
            start_col};
}

} // namespace ghoti::syntax
