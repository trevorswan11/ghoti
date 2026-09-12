#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include "helpers/codegen.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

TEST_CASE("@embed builtin reads file contents at compile-time") {
    tempfile embedded{"ghoti_test_embed"};
    {
        std::ofstream out{embedded.path};
        fmt::print(out, "Hello from embedded file!");
    }

    const auto source{fmt::format(R"(
        pub const main := fn(): i32 {{
            const data := @embed("{}");
            return @intCast(i32, data.len);
        }};
    )",
                                  embedded.path.generic_string())};
    CHECK(helpers::compile_and_run(source) == 25);
}

TEST_CASE("multiple @embed calls use in-memory cache") {
    tempfile embedded{"ghoti_test_embed_cache"};
    {
        std::ofstream out{embedded.path};
        out << "CacheTestBytes";
    }
    const auto path_str{embedded.path.generic_string()};

    const auto source{fmt::format(R"(
        pub const main := fn(): i32 {{
            const a := @embed("{}");
            const b := @embed("{}");
            return @intCast(i32, a.len) + @intCast(i32, b.len);
        }};
    )",
                                  path_str,
                                  path_str)};
    CHECK(helpers::compile_and_run(source) == 28);
}

} // namespace ghoti::tests
