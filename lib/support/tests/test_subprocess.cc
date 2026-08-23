#include <catch2/catch_test_macros.hpp>

#include "support/subprocess.hh"

namespace ghoti::tests {

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
