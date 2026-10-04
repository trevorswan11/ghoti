#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::run_cfg;
using helpers::selected;

TEST_CASE("cfg: a selected @cfg arm is spliced in, the rest are deleted") {
    constexpr std::string_view src{R"(
        @cfg(ptr_bits >= 8)  { let kept = 1; }
        else                 { let dropped = 2; }
    )"};
    CHECK(run_cfg(src).codes.empty());
    CHECK(selected(src, "kept"));
    CHECK_FALSE(selected(src, "dropped"));
}

TEST_CASE("cfg: else-@cfg chains pick exactly one arm") {
    constexpr std::string_view src{R"(
        @cfg(ptr_bits == 7)   { let a = 1; }
        else @cfg(ptr_bits == 9)  { let b = 2; }
        else                  { let c = 3; }
    )"};
    CHECK(run_cfg(src).codes.empty());
    CHECK(selected(src, "c"));
    CHECK_FALSE(selected(src, "a"));
    CHECK_FALSE(selected(src, "b"));
}

TEST_CASE("cfg: @cfgValue predicate form is a bool constant") {
    constexpr std::string_view src{R"(
        const IS_64 = @cfgValue(ptr_bits == 64);
        @cfg(IS_64)  { const wide = 1; }
        else         { const narrow = 1; }
    )"};
    CHECK(run_cfg(src).codes.empty());
}

TEST_CASE("cfg: @cfgValue constants may reference each other acyclically") {
    constexpr std::string_view src{R"(
        const A = @cfgValue(ptr_bits >= 32);
        const B = @cfgValue(A);
        const C = @cfgValue(A and B);
        @cfg(C) { const ok = 1; } else { const no = 1; }
    )"};
    CHECK(run_cfg(src).codes.empty());
}

TEST_CASE("cfg: a @cfgValue cycle is a hard error") {
    constexpr std::string_view src{R"(
        const A = @cfgValue(B);
        const B = @cfgValue(A);
        @cfg(A) { const x = 1; } else { const y = 1; }
    )"};
    CHECK(run_cfg(src).has_code(sema::error::CFG_VALUE_CYCLE));
}

TEST_CASE("cfg: strings are not accepted in predicate comparisons") {
    constexpr std::string_view src{R"( @cfg(os == "linux") { let x = 1; } else { let y = 1; } )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::CFG_COMPARISON_TYPE_MISMATCH));
    CHECK(out.any_message_contains("not strings"));
}

TEST_CASE("cfg: an unknown enum member is rejected with a suggestion") {
    constexpr std::string_view src{R"( @cfg(os == .linx) { let x = 1; } else { let y = 1; } )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::CFG_UNKNOWN_ENUM_MEMBER));
    CHECK(out.any_message_contains("did you mean '.linux'"));
}

TEST_CASE("cfg: comparing an atom to an integer is a type error") {
    constexpr std::string_view src{R"( @cfg(os == 3) { let x = 1; } else { let y = 1; } )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::CFG_COMPARISON_TYPE_MISMATCH));
    CHECK(out.any_message_contains("cannot be compared with an integer"));
}

TEST_CASE("cfg: ordered comparison on an enum atom is rejected") {
    constexpr std::string_view src{R"( @cfg(os < .linux) { let x = 1; } else { let y = 1; } )"};
    CHECK(run_cfg(src).has_code(sema::error::CFG_COMPARISON_TYPE_MISMATCH));
}

TEST_CASE("cfg: an unknown atom names the valid set") {
    constexpr std::string_view src{
        R"( @cfg(platform == .linux) { let x = 1; } else { let y = 1; } )"};
    const auto out{run_cfg(src)};
    CHECK(out.has_code(sema::error::CFG_UNKNOWN_ATOM));
    CHECK(out.any_message_contains("os, arch, abi, family, endian, ptr_bits"));
}

TEST_CASE("cfg: a predicate can only reach into a module it imports") {
    constexpr std::string_view src{R"( @cfg(sys.is_windows) { let x = 1; } else { let y = 1; } )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::CFG_ILLEGAL_CFG_VALUE_REFERENCE));
    CHECK(out.any_message_contains("'sys' in 'sys.is_windows' isn't a module import"));
}

TEST_CASE("cfg: another module's @cfgValue constant must be public") {
    const auto out{
        run_cfg(R"(
        import "sys.gh" as sys;
        @cfg(sys.WIDE) { let x = 1; }
    )",
                {helpers::mock_file{"sys.gh", "const WIDE = @cfgValue(ptr_bits >= 32);"}})};
    CHECK(out.has_code(sema::error::CFG_ILLEGAL_CFG_VALUE_REFERENCE));
    CHECK(out.any_message_contains("'sys.WIDE' isn't a public `@cfgValue` constant"));
}

TEST_CASE("cfg: past the first module, a predicate only follows public imports") {
    const auto out{
        run_cfg(R"(
        import "outer.gh" as outer;
        @cfg(outer.inner.WIDE) { let x = 1; }
    )",
                {
                    helpers::mock_file{"outer.gh", R"(import "inner.gh" as inner;)"},
                    helpers::mock_file{"inner.gh", "pub const WIDE = @cfgValue(ptr_bits >= 32);"},
                })};
    CHECK(out.has_code(sema::error::CFG_ILLEGAL_CFG_VALUE_REFERENCE));
    CHECK(out.any_message_contains("'inner' in 'outer.inner.WIDE' isn't a public module import"));
}

TEST_CASE("cfg: @cfgValue constants that depend on each other across modules are a cycle") {
    const auto out{run_cfg(R"(
        import "a.gh" as a;
        @cfg(a.X) { let x = 1; }
    )",
                           {
                               helpers::mock_file{"a.gh", R"(
                                   import "b.gh" as b;
                                   pub const X = @cfgValue(b.Y);
                               )"},
                               helpers::mock_file{"b.gh", R"(
                                   import "a.gh" as a;
                                   pub const Y = @cfgValue(a.X);
                               )"},
                           })};
    CHECK(out.has_code(sema::error::CFG_VALUE_CYCLE));
}

TEST_CASE("cfg: @cfgValue guard arms must agree in type") {
    constexpr std::string_view src{R"(
        const BAD = @cfgValue(
            ptr_bits == 64 => 8,
            _              => false,
        );
        @cfg(ptr_bits == 64) { let x = 1; } else { let y = 1; }
    )"};
    CHECK(run_cfg(src).has_code(sema::error::CFG_VALUE_GUARD_TYPE_MISMATCH));
}

TEST_CASE("cfg: @cfgValue guard form requires a fallback") {
    constexpr std::string_view src{R"(
        const BAD = @cfgValue(ptr_bits == 64 => 8, ptr_bits == 32 => 4);
        @cfg(ptr_bits == 64) { let x = 1; } else { let y = 1; }
    )"};
    CHECK(run_cfg(src).has_code(sema::error::CFG_VALUE_MISSING_FALLBACK));
}

TEST_CASE("cfg: a taken @cfgValue guard may diverge via @compileError") {
    constexpr std::string_view src{R"(
        const SYS = @cfgValue(
            ptr_bits == 64 => 1,
            _              => @compileError("unsupported pointer width"),
        );
        @cfg(ptr_bits == 64) { let x = 1; } else { let y = 1; }
    )"};
    // ptr_bits is 64 on every host I can test on
    CHECK(run_cfg(src).codes.empty());
}

TEST_CASE("cfg: @compileError in a selected arm fires eagerly") {
    constexpr std::string_view src{R"(
        @cfg(ptr_bits >= 8) { @compileError("this target is not supported"); }
        else                { let unused = 1; }
    )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::COMPILE_ERROR_REACHED));
    CHECK(out.any_message_contains("this target is not supported"));
}

TEST_CASE("cfg: @compileError in a pruned arm does nothing") {
    constexpr std::string_view src{R"(
        @cfg(ptr_bits >= 8) { let fine = 1; }
        else                { @compileError("never reached"); }
    )"};
    CHECK(run_cfg(src).codes.empty());
}

TEST_CASE("cfg: @compileError needs a comptime-known message") {
    constexpr std::string_view src{R"(
        @cfg(ptr_bits >= 8) { @compileError(some_runtime_value); }
        else                { let unused = 1; }
    )"};
    CHECK(run_cfg(src).has_code(sema::error::COMPILE_ERROR_NON_CONSTANT_MESSAGE));
}

} // namespace ghoti::tests
