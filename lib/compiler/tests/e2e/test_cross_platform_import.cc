#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// `*_via_api` wraps  a bogus named-library extern and must be pruned
constexpr std::string_view PLAT_LINUX{R"(
    extern("ghoti_fake_linux_lib") const FakeLinuxApi: fn(): i32;
    pub const linux_tag := fn(): i32 { return 7; };
    pub const linux_via_api := fn(): i32 { return FakeLinuxApi(); };
)"};

constexpr std::string_view PLAT_DARWIN{R"(
    extern("ghoti_fake_darwin_lib") const FakeDarwinApi: fn(): i32;
    pub const darwin_tag := fn(): i32 { return 7; };
    pub const darwin_via_api := fn(): i32 { return FakeDarwinApi(); };
)"};

constexpr std::string_view PLAT_WINDOWS{R"(
    extern("ghoti_fake_windows_lib") const FakeWindowsApi: fn(): i32;
    pub const windows_tag := fn(): i32 { return 7; };
    pub const windows_via_api := fn(): i32 { return FakeWindowsApi(); };
)"};

} // namespace

TEST_CASE("E2E: importing every platform module still links on the host") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "plat_linux.gh" as plat_linux;
            import "plat_darwin.gh" as plat_darwin;
            import "plat_windows.gh" as plat_windows;

            const plat_tag := fn(): i32 {
                if constexpr (@targetOs() == .linux) {
                    return plat_linux::linux_tag();
                } else if constexpr (@targetOs() == .macos) {
                    return plat_darwin::darwin_tag();
                } else {
                    return plat_windows::windows_tag();
                };
            };

            pub const main := fn(): i32 {
                return plat_tag();
            };
        )",
        {
            helpers::mock_file{"plat_linux.gh", PLAT_LINUX, "plat_linux"},
            helpers::mock_file{"plat_darwin.gh", PLAT_DARWIN, "plat_darwin"},
            helpers::mock_file{"plat_windows.gh", PLAT_WINDOWS, "plat_windows"},
        })};

    CHECK(exit_code == 7);
}

} // namespace ghoti::tests
