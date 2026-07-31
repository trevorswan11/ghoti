#include "compiler/syntax/keywords.hh"

#include <string_view>

#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::syntax {

namespace {

constexpr auto ALL_KEYWORDS{string_utils::make_constexpr_map<token_type_t>(keywords::FN,
                                                                           keywords::VAR,
                                                                           keywords::CONSTANT,
                                                                           keywords::CONSTEXPR,
                                                                           keywords::STRUCT,
                                                                           keywords::ENUM,
                                                                           keywords::UNION,
                                                                           keywords::BOOLEAN_TRUE,
                                                                           keywords::BOOLEAN_FALSE,
                                                                           keywords::IF,
                                                                           keywords::ELSE,
                                                                           keywords::DO,
                                                                           keywords::MATCH,
                                                                           keywords::RETURN,
                                                                           keywords::DEFER,
                                                                           keywords::LOOP,
                                                                           keywords::FOR,
                                                                           keywords::WHILE,
                                                                           keywords::CONTINUE,
                                                                           keywords::BREAK,
                                                                           keywords::IMPORT,
                                                                           keywords::I32,
                                                                           keywords::I64,
                                                                           keywords::ISIZE,
                                                                           keywords::U32,
                                                                           keywords::U64,
                                                                           keywords::USIZE,
                                                                           keywords::F32,
                                                                           keywords::F64,
                                                                           keywords::U8,
                                                                           keywords::BOOL,
                                                                           keywords::VOID,
                                                                           keywords::TYPE,
                                                                           keywords::AUTO,
                                                                           keywords::OPAQUE,
                                                                           keywords::AS,
                                                                           keywords::PUBLIC,
                                                                           keywords::EXTERN,
                                                                           keywords::EXPORT,
                                                                           keywords::VOLATILE,
                                                                           keywords::MUT_VOLATILE,
                                                                           keywords::PACKED,
                                                                           keywords::NORETURN,
                                                                           keywords::NULLPTR,
                                                                           keywords::USING,
                                                                           keywords::TEST,
                                                                           keywords::UNDEFINED,
                                                                           keywords::UNREACHABLE)};

} // namespace

auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    return ALL_KEYWORDS.get_opt(sv).materialize();
}

} // namespace ghoti::syntax
