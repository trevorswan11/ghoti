#pragma once

#include <array>
#include <string_view>

#include <stdx/enum.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

// Every `@builtin` but the special forms `@cfg` / `@cfgValue`, which the parser handles itself.
// They all lex as one `token_type_t::BUILTIN`; the spelling says which
enum class builtin_t : u8 {
    ALIGN_CAST,
    PTR_CAST,
    BIT_CAST,
    CONST_CAST,
    VOLATILE_CAST,
    AS,
    INT_CAST,
    TRUNCATE,
    BOOL_FROM_INT,
    INT_FROM_BOOL,
    BACKING_INT,
    FROM_BACKING_INT,
    INT_FROM_FLOAT,
    FLOAT_FROM_INT,
    FLOAT_CAST,
    INT_FROM_PTR,
    PTR_FROM_INT,
    SLICE_FROM_PTR,
    FIELD_PARENT_PTR,
    ALIGN_OF,
    SIZE_OF,
    BIT_SIZE_OF,
    TYPE_OF,
    THIS,
    TAG_NAME,
    TYPE_NAME,
    TYPE_INFO,
    HAS_FIELD,
    FIELD_TYPE,
    FIELD,
    INT,
    FLOAT,
    POINTER,
    REFERENCE,
    SLICE,
    ARRAY,
    VECTOR,
    FN,
    FIELD_DEFAULT,
    STRUCT,
    UNION,
    ENUM,
    MEMCPY,
    MEMSET,
    MEMMOVE,
    MUL_ADD,
    CLZ,
    CTZ,
    POPCOUNT,
    ABS,
    SQRT,
    SIN,
    COS,
    TAN,
    EXP,
    EXP2,
    LOG,
    LOG2,
    LOG10,
    FLOOR,
    CEIL,
    MIN,
    MAX,
    DIV_TRUNC,
    DIV_FLOOR,
    DIV_EXACT,
    REM,
    MOD,
    ADD_WITH_OVERFLOW,
    SUB_WITH_OVERFLOW,
    MUL_WITH_OVERFLOW,
    SHL_WITH_OVERFLOW,
    C_VA_START,
    C_VA_ARG,
    C_VA_COPY,
    C_VA_END,
    TARGET_OS,
    TARGET_ARCH,
    TARGET_TRIPLE,
    TARGET_ABI,
    TARGET_PTR_BITS,
    TARGET_ENDIAN,
    TARGET_FAMILY,
    OPTIMIZE_MODE,
    RUNTIME_SAFETY,
    SET_RUNTIME_SAFETY,
    BRANCH_HINT,
    SET_EVAL_RECURSION_LIMIT,
    SET_EVAL_UNROLL_LIMIT,
    EXPORT,
    PANIC,
    TRAP,
    FN_CTX,
    SRC,
    EXPECT,
    REQUIRE,
    SKIP,
    IMPLEMENTS,
    ASSERT,
    VERIFY,
    DYN_CAST,
    ATOMIC_LOAD,
    ATOMIC_STORE,
    ATOMIC_RMW,
    CMPXCHG_WEAK,
    CMPXCHG_STRONG,
    FENCE,
    COMPILE_ERROR,
    EMBED,
    CALL,
    ENUM_LITERAL,
    LLVM_INTRINSIC,
    SPLAT,
    REDUCE,
    SHUFFLE,
    SELECT,
    MASKED_LOAD,
    MASKED_STORE,
    GATHER,
    SCATTER,
    ENTRY_MAIN,
    ENTRY_TEST_RUNNER,
    TEST_FUNCTIONS,
    RETURN_ADDRESS,
};

// A builtin's spelling and kind
struct builtin_def {
    std::string_view name;
    builtin_t        kind;
};

namespace builtins {

constexpr builtin_def ALIGN_CAST{"@alignCast", builtin_t::ALIGN_CAST};
constexpr builtin_def PTR_CAST{"@ptrCast", builtin_t::PTR_CAST};
constexpr builtin_def BIT_CAST{"@bitCast", builtin_t::BIT_CAST};
constexpr builtin_def CONST_CAST{"@constCast", builtin_t::CONST_CAST};
constexpr builtin_def VOLATILE_CAST{"@volatileCast", builtin_t::VOLATILE_CAST};
constexpr builtin_def AS{"@as", builtin_t::AS};
constexpr builtin_def INT_CAST{"@intCast", builtin_t::INT_CAST};
constexpr builtin_def TRUNCATE{"@truncate", builtin_t::TRUNCATE};
constexpr builtin_def BOOL_FROM_INT{"@boolFromInt", builtin_t::BOOL_FROM_INT};
constexpr builtin_def INT_FROM_BOOL{"@intFromBool", builtin_t::INT_FROM_BOOL};
constexpr builtin_def BACKING_INT{"@backingInt", builtin_t::BACKING_INT};
constexpr builtin_def FROM_BACKING_INT{"@fromBackingInt", builtin_t::FROM_BACKING_INT};
constexpr builtin_def INT_FROM_FLOAT{"@intFromFloat", builtin_t::INT_FROM_FLOAT};
constexpr builtin_def FLOAT_FROM_INT{"@floatFromInt", builtin_t::FLOAT_FROM_INT};
constexpr builtin_def FLOAT_CAST{"@floatCast", builtin_t::FLOAT_CAST};
constexpr builtin_def DYN_CAST{"@dynCast", builtin_t::DYN_CAST};

constexpr builtin_def INT_FROM_PTR{"@intFromPtr", builtin_t::INT_FROM_PTR};
constexpr builtin_def PTR_FROM_INT{"@ptrFromInt", builtin_t::PTR_FROM_INT};
constexpr builtin_def SLICE_FROM_PTR{"@sliceFromPtr", builtin_t::SLICE_FROM_PTR};
constexpr builtin_def FIELD_PARENT_PTR{"@fieldParentPtr", builtin_t::FIELD_PARENT_PTR};

constexpr builtin_def ALIGN_OF{"@alignOf", builtin_t::ALIGN_OF};
constexpr builtin_def SIZE_OF{"@sizeOf", builtin_t::SIZE_OF};
constexpr builtin_def BIT_SIZE_OF{"@bitSizeOf", builtin_t::BIT_SIZE_OF};
constexpr builtin_def TYPE_OF{"@TypeOf", builtin_t::TYPE_OF};
constexpr builtin_def THIS{"@This", builtin_t::THIS};
constexpr builtin_def TAG_NAME{"@tagName", builtin_t::TAG_NAME};
constexpr builtin_def TYPE_NAME{"@typeName", builtin_t::TYPE_NAME};
constexpr builtin_def TYPE_INFO{"@typeInfo", builtin_t::TYPE_INFO};
constexpr builtin_def HAS_FIELD{"@hasField", builtin_t::HAS_FIELD};
constexpr builtin_def FIELD_TYPE{"@fieldType", builtin_t::FIELD_TYPE};
constexpr builtin_def FIELD{"@field", builtin_t::FIELD};

constexpr builtin_def INT{"@Int", builtin_t::INT};
constexpr builtin_def FLOAT{"@Float", builtin_t::FLOAT};
constexpr builtin_def POINTER{"@Pointer", builtin_t::POINTER};
constexpr builtin_def REFERENCE{"@Reference", builtin_t::REFERENCE};
constexpr builtin_def SLICE{"@Slice", builtin_t::SLICE};
constexpr builtin_def ARRAY{"@Array", builtin_t::ARRAY};
constexpr builtin_def VECTOR{"@Vector", builtin_t::VECTOR};
constexpr builtin_def FN{"@Fn", builtin_t::FN};
constexpr builtin_def FIELD_DEFAULT{"@fieldDefault", builtin_t::FIELD_DEFAULT};
constexpr builtin_def STRUCT{"@Struct", builtin_t::STRUCT};
constexpr builtin_def UNION{"@Union", builtin_t::UNION};
constexpr builtin_def ENUM{"@Enum", builtin_t::ENUM};

constexpr builtin_def MEMCPY{"@memcpy", builtin_t::MEMCPY};
constexpr builtin_def MEMSET{"@memset", builtin_t::MEMSET};
constexpr builtin_def MEMMOVE{"@memmove", builtin_t::MEMMOVE};

constexpr builtin_def MUL_ADD{"@mulAdd", builtin_t::MUL_ADD};
constexpr builtin_def CLZ{"@clz", builtin_t::CLZ};
constexpr builtin_def CTZ{"@ctz", builtin_t::CTZ};
constexpr builtin_def POPCOUNT{"@popCount", builtin_t::POPCOUNT};
constexpr builtin_def ABS{"@abs", builtin_t::ABS};
constexpr builtin_def SQRT{"@sqrt", builtin_t::SQRT};
constexpr builtin_def SIN{"@sin", builtin_t::SIN};
constexpr builtin_def COS{"@cos", builtin_t::COS};
constexpr builtin_def TAN{"@tan", builtin_t::TAN};
constexpr builtin_def EXP{"@exp", builtin_t::EXP};
constexpr builtin_def EXP2{"@exp2", builtin_t::EXP2};
constexpr builtin_def LOG{"@log", builtin_t::LOG};
constexpr builtin_def LOG2{"@log2", builtin_t::LOG2};
constexpr builtin_def LOG10{"@log10", builtin_t::LOG10};
constexpr builtin_def FLOOR{"@floor", builtin_t::FLOOR};
constexpr builtin_def CEIL{"@ceil", builtin_t::CEIL};

constexpr builtin_def MIN{"@min", builtin_t::MIN};
constexpr builtin_def MAX{"@max", builtin_t::MAX};
constexpr builtin_def DIV_TRUNC{"@divTrunc", builtin_t::DIV_TRUNC};
constexpr builtin_def DIV_FLOOR{"@divFloor", builtin_t::DIV_FLOOR};
constexpr builtin_def DIV_EXACT{"@divExact", builtin_t::DIV_EXACT};
constexpr builtin_def REM{"@rem", builtin_t::REM};
constexpr builtin_def MOD{"@mod", builtin_t::MOD};
constexpr builtin_def ADD_WITH_OVERFLOW{"@addWithOverflow", builtin_t::ADD_WITH_OVERFLOW};
constexpr builtin_def SUB_WITH_OVERFLOW{"@subWithOverflow", builtin_t::SUB_WITH_OVERFLOW};
constexpr builtin_def MUL_WITH_OVERFLOW{"@mulWithOverflow", builtin_t::MUL_WITH_OVERFLOW};
constexpr builtin_def SHL_WITH_OVERFLOW{"@shlWithOverflow", builtin_t::SHL_WITH_OVERFLOW};

constexpr builtin_def C_VA_START{"@cVaStart", builtin_t::C_VA_START};
constexpr builtin_def C_VA_ARG{"@cVaArg", builtin_t::C_VA_ARG};
constexpr builtin_def C_VA_COPY{"@cVaCopy", builtin_t::C_VA_COPY};
constexpr builtin_def C_VA_END{"@cVaEnd", builtin_t::C_VA_END};

constexpr builtin_def TARGET_OS{"@targetOs", builtin_t::TARGET_OS};
constexpr builtin_def TARGET_ARCH{"@targetArch", builtin_t::TARGET_ARCH};
constexpr builtin_def TARGET_TRIPLE{"@targetTriple", builtin_t::TARGET_TRIPLE};
constexpr builtin_def TARGET_ABI{"@targetAbi", builtin_t::TARGET_ABI};
constexpr builtin_def TARGET_PTR_BITS{"@targetPtrBits", builtin_t::TARGET_PTR_BITS};
constexpr builtin_def TARGET_ENDIAN{"@targetEndian", builtin_t::TARGET_ENDIAN};
constexpr builtin_def TARGET_FAMILY{"@targetFamily", builtin_t::TARGET_FAMILY};
constexpr builtin_def OPTIMIZE_MODE{"@optimizeMode", builtin_t::OPTIMIZE_MODE};
constexpr builtin_def RUNTIME_SAFETY{"@runtimeSafety", builtin_t::RUNTIME_SAFETY};
constexpr builtin_def SET_RUNTIME_SAFETY{"@setRuntimeSafety", builtin_t::SET_RUNTIME_SAFETY};
constexpr builtin_def BRANCH_HINT{"@branchHint", builtin_t::BRANCH_HINT};

constexpr builtin_def SET_EVAL_RECURSION_LIMIT{"@setEvalRecursionLimit",
                                               builtin_t::SET_EVAL_RECURSION_LIMIT};
constexpr builtin_def SET_EVAL_UNROLL_LIMIT{"@setEvalUnrollLimit",
                                            builtin_t::SET_EVAL_UNROLL_LIMIT};
constexpr builtin_def EXPORT{"@export", builtin_t::EXPORT};

constexpr builtin_def PANIC{"@panic", builtin_t::PANIC};
constexpr builtin_def TRAP{"@trap", builtin_t::TRAP};

// The innermost enclosing function as a callable value, for self-recursion
constexpr builtin_def FN_CTX{"@fnCtx", builtin_t::FN_CTX};

constexpr builtin_def SRC{"@src", builtin_t::SRC};
constexpr builtin_def IMPLEMENTS{"@implements", builtin_t::IMPLEMENTS};
constexpr builtin_def EXPECT{"@expect", builtin_t::EXPECT};
constexpr builtin_def REQUIRE{"@require", builtin_t::REQUIRE};
constexpr builtin_def SKIP{"@skip", builtin_t::SKIP};

constexpr builtin_def ASSERT{"@assert", builtin_t::ASSERT};
constexpr builtin_def VERIFY{"@verify", builtin_t::VERIFY};

constexpr builtin_def ATOMIC_LOAD{"@atomicLoad", builtin_t::ATOMIC_LOAD};
constexpr builtin_def ATOMIC_STORE{"@atomicStore", builtin_t::ATOMIC_STORE};
constexpr builtin_def ATOMIC_RMW{"@atomicRmw", builtin_t::ATOMIC_RMW};
constexpr builtin_def CMPXCHG_WEAK{"@cmpxchgWeak", builtin_t::CMPXCHG_WEAK};
constexpr builtin_def CMPXCHG_STRONG{"@cmpxchgStrong", builtin_t::CMPXCHG_STRONG};
constexpr builtin_def FENCE{"@fence", builtin_t::FENCE};

constexpr builtin_def      COMPILE_ERROR{"@compileError", builtin_t::COMPILE_ERROR};
constexpr builtin_def      EMBED{"@embed", builtin_t::EMBED};
constexpr builtin_def      CALL{"@call", builtin_t::CALL};
constexpr builtin_def      ENUM_LITERAL{"@EnumLiteral", builtin_t::ENUM_LITERAL};
constexpr builtin_def      LLVM_INTRINSIC{"@llvmIntrinsic", builtin_t::LLVM_INTRINSIC};
constexpr builtin_def      SPLAT{"@splat", builtin_t::SPLAT};
constexpr builtin_def      REDUCE{"@reduce", builtin_t::REDUCE};
constexpr builtin_def      SHUFFLE{"@shuffle", builtin_t::SHUFFLE};
constexpr builtin_def      SELECT{"@select", builtin_t::SELECT};
constexpr builtin_def      MASKED_LOAD{"@maskedLoad", builtin_t::MASKED_LOAD};
constexpr builtin_def      MASKED_STORE{"@maskedStore", builtin_t::MASKED_STORE};
constexpr builtin_def      GATHER{"@gather", builtin_t::GATHER};
constexpr builtin_def      SCATTER{"@scatter", builtin_t::SCATTER};
constexpr builtin_def      ENTRY_MAIN{"@entryMain", builtin_t::ENTRY_MAIN};
constexpr builtin_def      ENTRY_TEST_RUNNER{"@entryTestRunner", builtin_t::ENTRY_TEST_RUNNER};
constexpr builtin_def      TEST_FUNCTIONS{"@testFunctions", builtin_t::TEST_FUNCTIONS};
constexpr builtin_def      RETURN_ADDRESS{"@returnAddress", builtin_t::RETURN_ADDRESS};
constexpr typed_identifier CFG{"@cfg", token_type_t::BUILTIN_CFG};
constexpr typed_identifier CFG_VALUE{"@cfgValue", token_type_t::BUILTIN_CFG_VALUE};

constexpr std::array SPECIAL_FORMS{CFG, CFG_VALUE};

} // namespace builtins

// Single source of truth for every `@builtin` spelling
constexpr std::array ALL_BUILTINS{
    builtins::ALIGN_CAST,
    builtins::PTR_CAST,
    builtins::BIT_CAST,
    builtins::CONST_CAST,
    builtins::VOLATILE_CAST,
    builtins::AS,
    builtins::INT_CAST,
    builtins::TRUNCATE,
    builtins::BOOL_FROM_INT,
    builtins::INT_FROM_BOOL,
    builtins::BACKING_INT,
    builtins::FROM_BACKING_INT,
    builtins::INT_FROM_FLOAT,
    builtins::FLOAT_FROM_INT,
    builtins::FLOAT_CAST,
    builtins::INT_FROM_PTR,
    builtins::PTR_FROM_INT,
    builtins::SLICE_FROM_PTR,
    builtins::FIELD_PARENT_PTR,
    builtins::ALIGN_OF,
    builtins::SIZE_OF,
    builtins::BIT_SIZE_OF,
    builtins::TYPE_OF,
    builtins::THIS,
    builtins::TAG_NAME,
    builtins::TYPE_NAME,
    builtins::TYPE_INFO,
    builtins::HAS_FIELD,
    builtins::FIELD_TYPE,
    builtins::FIELD,
    builtins::INT,
    builtins::FLOAT,
    builtins::POINTER,
    builtins::REFERENCE,
    builtins::SLICE,
    builtins::ARRAY,
    builtins::VECTOR,
    builtins::FN,
    builtins::FIELD_DEFAULT,
    builtins::STRUCT,
    builtins::UNION,
    builtins::ENUM,
    builtins::MEMCPY,
    builtins::MEMSET,
    builtins::MEMMOVE,
    builtins::MUL_ADD,
    builtins::CLZ,
    builtins::CTZ,
    builtins::POPCOUNT,
    builtins::ABS,
    builtins::SQRT,
    builtins::SIN,
    builtins::COS,
    builtins::TAN,
    builtins::EXP,
    builtins::EXP2,
    builtins::LOG,
    builtins::LOG2,
    builtins::LOG10,
    builtins::FLOOR,
    builtins::CEIL,
    builtins::MIN,
    builtins::MAX,
    builtins::DIV_TRUNC,
    builtins::DIV_FLOOR,
    builtins::DIV_EXACT,
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
    builtins::TARGET_OS,
    builtins::TARGET_ARCH,
    builtins::TARGET_TRIPLE,
    builtins::TARGET_ABI,
    builtins::TARGET_PTR_BITS,
    builtins::TARGET_ENDIAN,
    builtins::TARGET_FAMILY,
    builtins::OPTIMIZE_MODE,
    builtins::RUNTIME_SAFETY,
    builtins::SET_RUNTIME_SAFETY,
    builtins::BRANCH_HINT,
    builtins::SET_EVAL_RECURSION_LIMIT,
    builtins::SET_EVAL_UNROLL_LIMIT,
    builtins::EXPORT,
    builtins::PANIC,
    builtins::TRAP,
    builtins::FN_CTX,
    builtins::SRC,
    builtins::IMPLEMENTS,
    builtins::EXPECT,
    builtins::REQUIRE,
    builtins::SKIP,
    builtins::ASSERT,
    builtins::VERIFY,
    builtins::DYN_CAST,
    builtins::ATOMIC_LOAD,
    builtins::ATOMIC_STORE,
    builtins::ATOMIC_RMW,
    builtins::CMPXCHG_WEAK,
    builtins::CMPXCHG_STRONG,
    builtins::FENCE,
    builtins::COMPILE_ERROR,
    builtins::EMBED,
    builtins::CALL,
    builtins::ENUM_LITERAL,
    builtins::LLVM_INTRINSIC,
    builtins::SPLAT,
    builtins::REDUCE,
    builtins::SHUFFLE,
    builtins::SELECT,
    builtins::MASKED_LOAD,
    builtins::MASKED_STORE,
    builtins::GATHER,
    builtins::SCATTER,
    builtins::ENTRY_MAIN,
    builtins::ENTRY_TEST_RUNNER,
    builtins::TEST_FUNCTIONS,
    builtins::RETURN_ADDRESS,
};

[[nodiscard]] auto builtin_name(builtin_t builtin) noexcept -> std::string_view;
[[nodiscard]] auto builtin_from_name(std::string_view name) noexcept -> stdx::option<builtin_t>;
// A `@builtin` or special form's token type
[[nodiscard]] auto builtin_token_opt(std::string_view name) noexcept -> stdx::option<token_type_t>;

} // namespace ghoti::syntax
