#include <string>

#include <catch2/catch_test_macros.hpp>

#include "support/string_utils.hh"
#include "support/subprocess.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("piped_process round-trips stdin to stdout through a real child process") {
    // `sort` reads every line from stdin and writes them back sorted once stdin closes
    piped_process proc{mock_argv{"sort"}};
    REQUIRE(proc.is_running());

    proc.stdin_stream() << "banana\napple\ncherry\n";
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);

    std::string line;
    std::getline(proc.stdout_stream(), line);
    string_utils::strip_trailing_cr(line);
    CHECK(line == "apple");
    std::getline(proc.stdout_stream(), line);
    string_utils::strip_trailing_cr(line);
    CHECK(line == "banana");
    std::getline(proc.stdout_stream(), line);
    string_utils::strip_trailing_cr(line);
    CHECK(line == "cherry");
}

TEST_CASE("Windows argument quoting") {
    CHECK(quote_arg_windows("plain") == "plain");
    CHECK(quote_arg_windows("has space") == R"("has space")");
    CHECK(quote_arg_windows(R"(say "hi")") == R"("say \"hi\"")");
    CHECK(quote_arg_windows(R"(C:\Users\Jane Doe\out.exe)") == R"("C:\Users\Jane Doe\out.exe")");
    CHECK(quote_arg_windows(R"(trailing\)") == R"(trailing\)");
    CHECK(quote_arg_windows(R"(trailing\ with space)") == R"("trailing\ with space")");
    CHECK(quote_arg_windows("") == R"("")");
}

} // namespace ghoti::tests
