#include "compiler/syntax/builtins.hh"

#include <array>
#include <string_view>

#include <stdx/fixed/enum_map.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::syntax {

namespace {

constexpr auto ALL_BUILTINS_BY_SV{
    string_utils::make_constexpr_map<token_type_t>(builtins::ALIGN_CAST,
                                                   builtins::PTR_CAST,
                                                   builtins::BIT_CAST,
                                                   builtins::CONST_CAST,
                                                   builtins::VOLATILE_CAST,
                                                   builtins::AS,
                                                   builtins::INT_FROM_PTR,
                                                   builtins::PTR_FROM_INT,
                                                   builtins::PTR_FROM_ARRAY,
                                                   builtins::SLICE_FROM_PTR,
                                                   builtins::FIELD_PARENT_PTR,
                                                   builtins::ALIGN_OF,
                                                   builtins::SIZE_OF,
                                                   builtins::TYPE_OF,
                                                   builtins::THIS,
                                                   builtins::TAG_NAME,
                                                   builtins::TYPE_NAME,
                                                   builtins::MEMCPY,
                                                   builtins::MEMSET,
                                                   builtins::MEMMOVE,
                                                   builtins::MUL_ADD,
                                                   builtins::CLZ,
                                                   builtins::CTZ,
                                                   builtins::POP_COUNT,
                                                   builtins::ABS,
                                                   builtins::MIN,
                                                   builtins::MAX,
                                                   builtins::DIV_TRUNC,
                                                   builtins::DIV_FLOOR,
                                                   builtins::REM,
                                                   builtins::MOD,
                                                   builtins::ADD_WITH_OVERFLOW,
                                                   builtins::SUB_WITH_OVERFLOW,
                                                   builtins::MUL_WITH_OVERFLOW,
                                                   builtins::SHL_WITH_OVERFLOW,
                                                   builtins::C_VA_START,
                                                   builtins::C_VA_ARG,
                                                   builtins::C_VA_COPY,
                                                   builtins::C_VA_END,
                                                   builtins::ALIGNAS,
                                                   builtins::TARGET_OS,
                                                   builtins::TARGET_ARCH,
                                                   builtins::TARGET_TRIPLE,
                                                   builtins::TARGET_ABI,
                                                   builtins::TARGET_PTR_BITS,
                                                   builtins::TARGET_ENDIAN,
                                                   builtins::TARGET_FAMILY,
                                                   builtins::SET_EVAL_RECURSION_LIMIT,
                                                   builtins::SET_MAIN_SYMBOL,
                                                   builtins::PANIC,
                                                   builtins::TRAP,
                                                   builtins::FN_CTX,
                                                   builtins::SRC,
                                                   builtins::EXPECT,
                                                   builtins::REQUIRE,
                                                   builtins::SKIP,
                                                   builtins::CFG,
                                                   builtins::CFG_VALUE,
                                                   builtins::COMPILE_ERROR)};

constexpr auto ALL_BUILTINS_BY_TT{[] -> auto {
    stdx::fixed::enum_map<token_type_t, stdx::option<std::string_view>> builtins;
    for (const auto& [name, tok] : ALL_BUILTINS_BY_SV) { builtins[tok] = name; }
    return builtins;
}()};

} // namespace

auto get_builtin_opt(token_type_t tt) noexcept -> stdx::option<std::string_view> {
    return ALL_BUILTINS_BY_TT[tt];
}

auto get_builtin_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    return ALL_BUILTINS_BY_SV.get_opt(sv).materialize();
}

} // namespace ghoti::syntax
