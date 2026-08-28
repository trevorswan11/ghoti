#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::enum_variants;
using helpers::run_cfg;
using helpers::struct_fields;

TEST_CASE("cfg: a selected struct @cfg block splices its fields at the group's position") {
    constexpr std::string_view src{R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits >= 8) { b: i32, c: i32 }
            d: i32,
        };
    )"};
    CHECK(run_cfg(src).codes.empty());
    CHECK(struct_fields(src, "S") == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("cfg: an unselected struct @cfg block contributes no fields; else wins") {
    constexpr std::string_view src{R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits == 7) { linux_only: i32 }
            else                { fallback: i32 }
            z: i32,
        };
    )"};
    CHECK(struct_fields(src, "S") == std::vector<std::string>{"a", "fallback", "z"});
}

TEST_CASE("cfg: the single-field @cfg form works in a struct body") {
    constexpr std::string_view src{R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits >= 8) b: i32,
            c: i32,
        };
    )"};
    CHECK(struct_fields(src, "S") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("cfg: a @cfgValue constant gates struct fields") {
    constexpr std::string_view src{R"(
        const IS_64 := @cfgValue(ptr_bits == 64);
        const S := struct {
            common: i32,
            @cfg(IS_64) { wide: i32 }
        };
    )"};
    const auto                 fields{struct_fields(src, "S")};
    CHECK(fields.front() == "common");
    CHECK(fields.size() == 2);
    CHECK(fields.back() == "wide");
}

TEST_CASE("cfg: block and single forms both work in an enum body") {
    constexpr std::string_view src{R"(
        const Tag := enum {
            A,
            @cfg(ptr_bits >= 8) { B, C }
            @cfg(ptr_bits >= 8) D,
            F,
        };
    )"};
    CHECK(run_cfg(src).codes.empty());
    CHECK(enum_variants(src, "Tag") == std::vector<std::string>{"A", "B", "C", "D", "F"});
}

TEST_CASE("cfg: a bad predicate inside an aggregate @cfg is reported") {
    constexpr std::string_view src{R"(
        const S := struct {
            a: i32,
            @cfg(os == .linx) { b: i32 }
        };
    )"};
    CHECK(run_cfg(src).has_code(sema::error::CFG_UNKNOWN_ENUM_MEMBER));
}

} // namespace ghoti::tests
