#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

using operator_t = typed_identifier;

namespace operators {

constexpr operator_t ASSIGN{"=", token_type_t::ASSIGN};
constexpr operator_t WALRUS{":=", token_type_t::WALRUS};
constexpr operator_t PLUS{"+", token_type_t::PLUS};
constexpr operator_t PLUS_ASSIGN{"+=", token_type_t::PLUS_ASSIGN};
constexpr operator_t MINUS{"-", token_type_t::MINUS};
constexpr operator_t MINUS_ASSIGN{"-=", token_type_t::MINUS_ASSIGN};
constexpr operator_t STAR{"*", token_type_t::STAR};
constexpr operator_t STAR_ASSIGN{"*=", token_type_t::STAR_ASSIGN};
constexpr operator_t SLASH{"/", token_type_t::SLASH};
constexpr operator_t SLASH_ASSIGN{"/=", token_type_t::SLASH_ASSIGN};
constexpr operator_t PERCENT{"%", token_type_t::PERCENT};
constexpr operator_t PERCENT_ASSIGN{"%=", token_type_t::PERCENT_ASSIGN};
constexpr operator_t BANG{"!", token_type_t::BANG};
constexpr operator_t AND_MUT{"&mut", token_type_t::AND_MUT};
constexpr operator_t CARET_MUT{"^mut", token_type_t::CARET_MUT};

constexpr operator_t BW_AND{"&", token_type_t::BW_AND};
constexpr operator_t BW_AND_ASSIGN{"&=", token_type_t::BW_AND_ASSIGN};
constexpr operator_t BW_OR{"|", token_type_t::BW_OR};
constexpr operator_t BW_OR_ASSIGN{"|=", token_type_t::BW_OR_ASSIGN};
constexpr operator_t SHL{"<<", token_type_t::SHL};
constexpr operator_t SHL_ASSIGN{"<<=", token_type_t::SHL_ASSIGN};
constexpr operator_t SHR{">>", token_type_t::SHR};
constexpr operator_t SHR_ASSIGN{">>=", token_type_t::SHR_ASSIGN};
constexpr operator_t NOT{"~", token_type_t::NOT};
constexpr operator_t NOT_ASSIGN{"~=", token_type_t::NOT_ASSIGN};
constexpr operator_t CARET{"^", token_type_t::CARET};
constexpr operator_t XOR_ASSIGN{"^=", token_type_t::XOR_ASSIGN};

constexpr operator_t BOOLEAN_AND{"and", token_type_t::BOOLEAN_AND};
constexpr operator_t BOOLEAN_OR{"or", token_type_t::BOOLEAN_OR};
constexpr operator_t LT{"<", token_type_t::LT};
constexpr operator_t LT_EQ{"<=", token_type_t::LT_EQ};
constexpr operator_t GT{">", token_type_t::GT};
constexpr operator_t GT_EQ{">=", token_type_t::GT_EQ};
constexpr operator_t EQ{"==", token_type_t::EQ};
constexpr operator_t NEQ{"!=", token_type_t::NEQ};

constexpr operator_t ELLIPSIS{"...", token_type_t::ELLIPSIS};
constexpr operator_t COLON_COLON{"::", token_type_t::COLON_COLON};
constexpr operator_t DOT{".", token_type_t::DOT};
constexpr operator_t DOT_DOT{"..", token_type_t::DOT_DOT};
constexpr operator_t DOT_DOT_EQ{"..=", token_type_t::DOT_DOT_EQ};
constexpr operator_t FAT_ARROW{"=>", token_type_t::FAT_ARROW};
constexpr operator_t COMMENT{"//", token_type_t::COMMENT};
constexpr operator_t MULTILINE_STRING{"\\\\", token_type_t::MULTILINE_STRING};
constexpr operator_t NULL_TERMINATED{":0", token_type_t::NULL_TERMINATED};

} // namespace operators

[[nodiscard]] auto max_operator_length() noexcept -> usize;
[[nodiscard]] auto get_operator_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;
[[nodiscard]] auto get_operator_opt(token_type_t tt) noexcept -> stdx::option<std::string_view>;

} // namespace ghoti::syntax
