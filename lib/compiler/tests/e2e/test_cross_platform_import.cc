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

// Mirrors `lib/std/os/os.gh`: a `@cfg(os)` backend select that flat-re-exports
constexpr std::string_view OS_BACKEND_DARWIN{R"(
    pub const Errno := enum : i32 { OK = 0, NOPE = 1, _ };
    pub using Handle = i32;
    pub const answer := fn(): i32 { return 7; };
    pub const WHENCE_END: i32 = 2;
)"};

constexpr std::string_view OS_BACKEND_WINDOWS{R"(
    pub const Errno := enum : u32 { OK = 0u32, NOPE = 1u32, _ };
    pub using Handle = ^mut opaque;
    pub const answer := fn(): i32 { return 7; };
    pub const WHENCE_END: i32 = 2;
)"};

constexpr std::string_view OS_SELECT{R"(
    @cfg(os == .macos) import "os_darwin.gh" as backend;
    else @cfg(os == .windows) import "os_windows.gh" as backend;
    else @compileError("unsupported target OS");

    pub using Handle = backend::Handle;
    pub using Errno = backend::Errno;
    pub using answer = backend::answer;
    pub const WHENCE_END := backend::WHENCE_END;
)"};

constexpr std::string_view OS_STD{R"( pub import "os_select.gh" as os; )"};

TEST_CASE("E2E: a @cfg-selected backend flat-re-exports its type / fn / const through `std`") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "std.gh" as std;
            pub const main := fn(): i32 {
                const e: std::os::Errno = std::os::Errno.NOPE;
                const w := @as(i32, std::os::WHENCE_END);
                const a := std::os::answer();
                return if (e == std::os::Errno.NOPE and w == 2) a else 1;
            };
        )",
        {
            helpers::mock_file{"os_darwin.gh", OS_BACKEND_DARWIN, "os_darwin"},
            helpers::mock_file{"os_windows.gh", OS_BACKEND_WINDOWS, "os_windows"},
            helpers::mock_file{"os_select.gh", OS_SELECT, "os_select"},
            helpers::mock_file{"std.gh", OS_STD, "std"},
        })};

    CHECK(exit_code == 7);
}

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
