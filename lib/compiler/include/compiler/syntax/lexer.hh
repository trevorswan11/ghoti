#pragma once

#include <iterator>
#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "compiler/syntax/trvia.hh"

namespace ghoti::syntax {

class lexer {
  public:
    class iterator {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = token_t;
        using difference_type   = idiff;
        using pointer           = const token_t*;
        using reference         = const token_t&;

      public:
        constexpr iterator(lexer& lexer, const token_t& current_token)
            : lexer_{lexer}, current_token_{current_token} {}

        constexpr auto operator++() -> iterator& {
            current_token_ = lexer_.advance();
            return *this;
        }

        constexpr auto operator*() const noexcept -> reference { return current_token_; }
        constexpr auto operator->() const noexcept -> pointer { return &current_token_; }

        [[nodiscard]] constexpr auto operator==(std::default_sentinel_t) const noexcept -> bool {
            return current_token_.type == token_type_t::END;
        }

      private:
        lexer&  lexer_;
        token_t current_token_;
    };

    class snapshot {
      public:
        constexpr explicit snapshot(const lexer& l) noexcept
            : pos_{l.pos_}, peek_pos_{l.peek_pos_}, current_byte_{l.current_byte_},
              line_no_{l.line_no_}, col_no_{l.col_no_} {}

      private:
        usize pos_;
        usize peek_pos_;
        char  current_byte_;
        usize line_no_;
        usize col_no_;

        friend class lexer;
    };

  public:
    lexer() noexcept = default;
    explicit lexer(std::string_view input) noexcept : input_{input} { read_character(); }

    auto reset(std::string_view input = {}) noexcept -> void;
    auto advance() noexcept -> token_t;
    auto advance_enriched() noexcept -> enriched_token;

    auto        begin() noexcept -> iterator { return iterator{*this, advance()}; }
    static auto end() noexcept -> std::default_sentinel_t { return std::default_sentinel; }

  private:
    auto        skip_whitespace() noexcept -> void;
    static auto lu_builtin(std::string_view ident) noexcept -> token_type_t;
    static auto lu_ident(std::string_view ident) noexcept -> token_type_t;

    // Reads n characters from the input stream
    auto               read_character(u8 n = 1) noexcept -> void;
    [[nodiscard]] auto read_operator() const noexcept -> stdx::option<token_t>;
    auto               read_ident(bool builtin) noexcept -> std::string_view;
    auto               read_number() noexcept -> token_t;
    auto               read_escape() noexcept -> char;
    auto               read_string() noexcept -> token_t;
    auto               read_multiline_string() noexcept -> token_t;
    auto               read_byte_literal() noexcept -> token_t;
    auto               read_comment() noexcept -> token_t;

    // Sets the lexer to the snapshot, very cheap operation.
    constexpr auto restore(const snapshot& state) noexcept -> void {
        pos_          = state.pos_;
        peek_pos_     = state.peek_pos_;
        current_byte_ = state.current_byte_;
        line_no_      = state.line_no_;
        col_no_       = state.col_no_;
    }

  private:
    std::string_view input_;
    usize            pos_{0};
    usize            peek_pos_{0};
    char             current_byte_{0};

    usize line_no_{0};
    usize col_no_{0};

    friend class parser;
};

} // namespace ghoti::syntax
