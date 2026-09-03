#pragma once

#include <array>
#include <string_view>

#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

using keyword_t = typed_identifier;

namespace keywords {

constexpr keyword_t FN{"fn", token_type_t::FUNCTION};
constexpr keyword_t VAR{"var", token_type_t::VAR};
constexpr keyword_t CONSTANT{"const", token_type_t::CONSTANT};
constexpr keyword_t CONSTEXPR{"constexpr", token_type_t::CONSTEXPR};
constexpr keyword_t STRUCT{"struct", token_type_t::STRUCT};
constexpr keyword_t ENUM{"enum", token_type_t::ENUM};
constexpr keyword_t UNION{"union", token_type_t::UNION};
constexpr keyword_t BOOLEAN_TRUE{"true", token_type_t::BOOLEAN_TRUE};
constexpr keyword_t BOOLEAN_FALSE{"false", token_type_t::BOOLEAN_FALSE};
constexpr keyword_t IF{"if", token_type_t::IF};
constexpr keyword_t ELSE{"else", token_type_t::ELSE};
constexpr keyword_t DO{"do", token_type_t::DO};
constexpr keyword_t MATCH{"match", token_type_t::MATCH};
constexpr keyword_t RETURN{"return", token_type_t::RETURN};
constexpr keyword_t DEFER{"defer", token_type_t::DEFER};
constexpr keyword_t LOOP{"loop", token_type_t::LOOP};
constexpr keyword_t FOR{"for", token_type_t::FOR};
constexpr keyword_t WHILE{"while", token_type_t::WHILE};
constexpr keyword_t CONTINUE{"continue", token_type_t::CONTINUE};
constexpr keyword_t BREAK{"break", token_type_t::BREAK};
constexpr keyword_t IMPORT{"import", token_type_t::IMPORT};
constexpr keyword_t I8{"i8", token_type_t::I8_TYPE};
constexpr keyword_t I16{"i16", token_type_t::I16_TYPE};
constexpr keyword_t I32{"i32", token_type_t::I32_TYPE};
constexpr keyword_t I64{"i64", token_type_t::I64_TYPE};
constexpr keyword_t ISIZE{"isize", token_type_t::ISIZE_TYPE};
constexpr keyword_t U16{"u16", token_type_t::U16_TYPE};
constexpr keyword_t U32{"u32", token_type_t::U32_TYPE};
constexpr keyword_t U64{"u64", token_type_t::U64_TYPE};
constexpr keyword_t USIZE{"usize", token_type_t::USIZE_TYPE};
constexpr keyword_t F32{"f32", token_type_t::F32_TYPE};
constexpr keyword_t F64{"f64", token_type_t::F64_TYPE};
constexpr keyword_t U8{"u8", token_type_t::U8_TYPE};
constexpr keyword_t BOOL{"bool", token_type_t::BOOL_TYPE};
constexpr keyword_t VOID{"void", token_type_t::VOID_TYPE};
constexpr keyword_t TYPE{"type", token_type_t::TYPE_TYPE};
constexpr keyword_t AUTO{"auto", token_type_t::AUTO_TYPE};
constexpr keyword_t OPAQUE{"opaque", token_type_t::OPAQUE_TYPE};
constexpr keyword_t AS{"as", token_type_t::AS};
constexpr keyword_t PUBLIC{"pub", token_type_t::PUBLIC};
constexpr keyword_t EXTERN{"extern", token_type_t::EXTERN};
constexpr keyword_t EXPORT{"export", token_type_t::EXPORT};
constexpr keyword_t THREADLOCAL{"threadlocal", token_type_t::THREADLOCAL};
constexpr keyword_t WEAK{"weak", token_type_t::WEAK};
constexpr keyword_t NAKED{"naked", token_type_t::NAKED};
constexpr keyword_t CALLCONV{"callconv", token_type_t::CALLCONV};
constexpr keyword_t VOLATILE{"volatile", token_type_t::VOLATILE};
constexpr keyword_t MUT{"mut", token_type_t::MUT};
constexpr keyword_t MOVE{"move", token_type_t::MOVE};
constexpr keyword_t PACKED{"packed", token_type_t::PACKED};
constexpr keyword_t NORETURN{"noreturn", token_type_t::NORETURN};
constexpr keyword_t NULLPTR{"nullptr", token_type_t::NULLPTR};
constexpr keyword_t USING{"using", token_type_t::USING};
constexpr keyword_t TEST{"test", token_type_t::TEST};
constexpr keyword_t IMPL{"impl", token_type_t::IMPL};
constexpr keyword_t INTERFACE{"interface", token_type_t::INTERFACE};
constexpr keyword_t DYN{"dyn", token_type_t::DYN};
constexpr keyword_t ASM{"asm", token_type_t::ASM};
constexpr keyword_t UNDEFINED{"undefined", token_type_t::UNDEFINED};
constexpr keyword_t UNREACHABLE{"unreachable", token_type_t::UNREACHABLE};

} // namespace keywords

[[nodiscard]] auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;
[[nodiscard]] auto get_keyword_opt(token_type_t tt) noexcept -> stdx::option<std::string_view>;

// Single source of truth for every reserved word
constexpr std::array ALL_KEYWORDS{
    keywords::FN,          keywords::VAR,          keywords::CONSTANT,
    keywords::CONSTEXPR,   keywords::STRUCT,       keywords::ENUM,
    keywords::UNION,       keywords::BOOLEAN_TRUE, keywords::BOOLEAN_FALSE,
    keywords::IF,          keywords::ELSE,         keywords::DO,
    keywords::MATCH,       keywords::RETURN,       keywords::DEFER,
    keywords::LOOP,        keywords::FOR,          keywords::WHILE,
    keywords::CONTINUE,    keywords::BREAK,        keywords::IMPORT,
    keywords::I8,          keywords::I16,          keywords::I32,
    keywords::I64,         keywords::ISIZE,        keywords::U16,
    keywords::U32,         keywords::U64,          keywords::USIZE,
    keywords::F32,         keywords::F64,          keywords::U8,
    keywords::BOOL,        keywords::VOID,         keywords::TYPE,
    keywords::AUTO,        keywords::OPAQUE,       keywords::AS,
    keywords::PUBLIC,      keywords::EXTERN,       keywords::EXPORT,
    keywords::THREADLOCAL, keywords::WEAK,         keywords::NAKED,
    keywords::CALLCONV,    keywords::VOLATILE,     keywords::MUT,
    keywords::MOVE,        keywords::PACKED,       keywords::NORETURN,
    keywords::NULLPTR,     keywords::USING,        keywords::TEST,
    keywords::UNDEFINED,   keywords::UNREACHABLE,  keywords::ASM,
    keywords::IMPL,        keywords::INTERFACE,    keywords::DYN,
};

constexpr std::array ALL_PRIMITIVES{
    keywords::I8.type,
    keywords::I16.type,
    keywords::I32.type,
    keywords::I64.type,
    keywords::ISIZE.type,
    keywords::U16.type,
    keywords::U32.type,
    keywords::U64.type,
    keywords::USIZE.type,
    keywords::F32.type,
    keywords::F64.type,
    keywords::U8.type,
    keywords::BOOL.type,
    keywords::VOID.type,
};

} // namespace ghoti::syntax
