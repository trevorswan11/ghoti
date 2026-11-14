#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/ostream.h>

#include "driver/clap/error.hh"
#include "driver/cmd/build/analyze.hh"
#include "support/path_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

auto write_source(const tempfile& file, std::string_view source) -> void {
    std::ofstream out{file.path};
    fmt::print(out, "{}", source);
}

[[nodiscard]] auto slurp(const std::filesystem::path& p) -> std::string {
    std::ifstream     in{p, std::ios::binary};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("analyze command execution") {
    SECTION("A missing input file is FILE_NOT_FOUND") {
        cmd::analyze_cmd cmd{{.input_path = "non_existent_file_12345.gh"}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("A well-formed program passes and writes nothing") {
        tempfile src_file{"test_analyze_ok.gh"};
        write_source(src_file, "pub const main = fn(): i32 {\n    return 3;\n};\n");

        std::ostringstream errors;
        cmd::analyze_cmd   cmd{{.input_path = src_file}, false, errors};
        REQUIRE(cmd.execute());
        CHECK(errors.view().empty());

        auto object{src_file.path};
        object.replace_extension(".o");
        CHECK_FALSE(path_utils::exists(object));
        object.replace_extension(".obj");
        CHECK_FALSE(path_utils::exists(object));
    }

    SECTION("A semantic error fails the analysis and is printed") {
        tempfile src_file{"test_analyze_bad.gh"};
        write_source(src_file, "pub const main = fn(): i32 {\n    return nope;\n};\n");

        std::ostringstream errors;
        cmd::analyze_cmd   cmd{{.input_path = src_file}, false, errors};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
        CHECK(errors.view().contains("UNDECLARED_IDENTIFIER"));
    }

    SECTION("--emit-gir and --emit-uir write their dumps") {
        tempfile src_file{"test_analyze_emit.gh"};
        tempfile gir_file{"test_analyze_emit.gir"};
        tempfile uir_file{"test_analyze_emit.uir"};
        write_source(src_file,
                     "pub const add = fn(a: i64, b: i64): i64 {\n    return a + b;\n};\n");

        cmd::analyze_cmd cmd{{
            .input_path    = src_file,
            .emit_gir_path = gir_file,
            .emit_uir_path = uir_file,
        }};
        REQUIRE(cmd.execute());
        CHECK(slurp(gir_file.path).contains("fn add("));
        CHECK(slurp(uir_file.path).contains("decl#0 VALUE add"));
    }

    SECTION("--test analyzes the tests without running them") {
        tempfile src_file{"test_analyze_tests.gh"};
        write_source(src_file, "test \"fails when run\" {\n    @require(1 == 2);\n}\n");

        cmd::analyze_cmd cmd{{.input_path = src_file}, true};
        REQUIRE(cmd.execute());
    }

    SECTION("--test checks the test entry a build would force") {
        tempfile src_file{"test_analyze_runner.gh"};
        write_source(src_file, "pub const test_runner = fn(): void {};\n");

        std::ostringstream errors;
        cmd::analyze_cmd   without_test{{.input_path = src_file}, false, errors};
        REQUIRE(without_test.execute());

        cmd::analyze_cmd as_test{{.input_path = src_file}, true, errors};
        CHECK(UNWRAP_ERR(as_test.execute()) == clap::error::COMPILATION_FAILED);
        CHECK_FALSE(errors.view().empty());
    }
}

} // namespace ghoti::tests
