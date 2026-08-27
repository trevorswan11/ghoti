#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "driver/clap/error.hh"
#include "driver/cmd/format/formatter.hh"
#include "driver/cmd/format/options.hh"
#include "support/string_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Formatter formats file to out_stream") {
    tempfile src_file{"unformatted.gh"};
    {
        std::ofstream out{src_file.path, std::ios::binary};
        fmt::print(out, "const x:i32=10;\n");
    }

    std::ostringstream out_stream;
    std::ostringstream err_stream;

    cmd::format::options opts{
        .input_paths = {src_file.path},
    };

    cmd::formatter fmt_cmd{opts, err_stream, out_stream};
    REQUIRE(fmt_cmd.execute());
    CHECK_FALSE(out_stream.view().empty());
    CHECK(out_stream.view() == "const x: i32 = 10;\n");
}

TEST_CASE("In-place formatting updates target file") {
    tempfile src_file{"to_format_inplace.gh"};
    {
        std::ofstream out{src_file.path, std::ios::binary};
        fmt::print(out, "const y:i32=42;\n");
    }

    std::ostringstream out_stream;
    std::ostringstream err_stream;

    cmd::format::options opts{
        .input_paths    = {src_file.path},
        .write_in_place = true,
    };

    cmd::formatter fmt_cmd{opts, err_stream, out_stream};
    REQUIRE(fmt_cmd.execute());

    std::ifstream in{src_file.path, std::ios::binary};
    auto          content{string_utils::read_stream(in)};
    CHECK(content == "const y: i32 = 42;\n");
}

TEST_CASE("Check mode fails on unformatted code and succeeds on formatted code") {
    tempfile src_file{"check_test.gh"};
    {
        std::ofstream out{src_file.path, std::ios::binary};
        fmt::print(out, "const z:i32=100;\n");
    }

    std::ostringstream out_stream;
    std::ostringstream err_stream;

    cmd::format::options opts{
        .input_paths = {src_file.path},
        .check_only  = true,
    };

    cmd::formatter fmt_cmd{opts, err_stream, out_stream};
    CHECK(UNWRAP_ERR(fmt_cmd.execute()) == clap::error::FORMATTING_FAILED);

    {
        std::ofstream out{src_file.path, std::ios::binary};
        fmt::print(out, "const z: i32 = 100;\n");
    }

    std::ostringstream out_stream2;
    std::ostringstream err_stream2;
    cmd::formatter     fmt_cmd2{opts, err_stream2, out_stream2};
    REQUIRE(fmt_cmd2.execute());
}

TEST_CASE("Syntax error in input file returns FORMATTING_FAILED") {
    tempfile src_file{"syntax_error.gh"};
    {
        std::ofstream out{src_file.path, std::ios::binary};
        fmt::print(out, "const z: i32 = ;\n");
    }

    std::ostringstream out_stream;
    std::ostringstream err_stream;

    cmd::format::options opts{
        .input_paths = {src_file.path},
    };

    cmd::formatter fmt_cmd{opts, err_stream, out_stream};
    CHECK(UNWRAP_ERR(fmt_cmd.execute()) == clap::error::FORMATTING_FAILED);
    CHECK_FALSE(err_stream.view().empty());
}

TEST_CASE("Non-existent input file returns FORMATTING_FAILED and warns error") {
    std::ostringstream out_stream;
    std::ostringstream err_stream;

    cmd::format::options opts{
        .input_paths = {"non_existent_file_99999.gh"},
    };

    cmd::formatter fmt_cmd{opts, err_stream, out_stream};
    CHECK(UNWRAP_ERR(fmt_cmd.execute()) == clap::error::FORMATTING_FAILED);
    CHECK_FALSE(err_stream.view().empty());
}

} // namespace ghoti::tests
