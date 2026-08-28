#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::run_cfg;

TEST_CASE("cfg: @cfg is evaluated inside an if block") {
    constexpr std::string_view src{R"(
        pub const f := fn(): i32 {
            if (true) {
                @cfg(ptr_bits >= 8) { @compileError("reached in if"); }
            }
            return 0;
        };
    )"};
    const auto                 out{run_cfg(src)};
    CHECK(out.has_code(sema::error::COMPILE_ERROR_REACHED));
    CHECK(out.any_message_contains("reached in if"));
}

TEST_CASE("cfg: @cfg is evaluated inside a match arm") {
    constexpr std::string_view src{R"(
        pub const f := fn(x: i32): i32 {
            match (x) {
                _ => {
                    @cfg(ptr_bits >= 8) { @compileError("reached in match"); }
                }
            }
            return 0;
        };
    )"};
    CHECK(run_cfg(src).any_message_contains("reached in match"));
}

TEST_CASE("cfg: @cfg is evaluated inside a loop body") {
    constexpr std::string_view src{R"(
        pub const f := fn(): i32 {
            while (true) {
                @cfg(ptr_bits >= 8) { @compileError("reached in loop"); }
            }
            return 0;
        };
    )"};
    CHECK(run_cfg(src).any_message_contains("reached in loop"));
}

TEST_CASE("cfg: a nested @cfg splices its selected arm and prunes the rest") {
    constexpr std::string_view src{R"(
        pub const f := fn(): i32 {
            if (true) {
                @cfg(ptr_bits >= 8) { const fine := 1; }
                else                { @compileError("pruned nested arm"); }
            }
            return 0;
        };
    )"};
    CHECK(run_cfg(src).codes.empty());
}

} // namespace ghoti::tests
