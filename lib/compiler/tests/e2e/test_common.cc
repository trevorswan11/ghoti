#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Pure noop function with loops") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): void {
            const a := [_]i32{};
            for (a) |_| {}
            while (false) {}
            do {} while (false);
            loop { break; }
        };
    )") == 0);
}

} // namespace ghoti::tests
