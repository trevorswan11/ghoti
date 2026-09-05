#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

using helpers::mock_file;

constexpr std::string_view RESULT{R"(
    pub constexpr Result := fn(T: type, E: type): type {
        return union { ok: T, err: E };
    };
)"};

constexpr std::string_view LEAF{R"(
    import "result.gh" as result;

    pub using Errno = i32;

    extern("C", "do_stat") const raw_stat: fn(out: ^mut Stat): i32;

    pub const Stat := extern struct {
        pub dev: i64,
        pub mode: i32,
        pub nlink: i32,
        pub ino: i64,
        pub atime: i64,
        pub mtime: i64,
        pub ctime: i64,
        pub blocks: i64,
        pub blksize: i32,
        pub flags: i32,
        pub spare: [2]i64,
    };

    constexpr R := fn(T: type): type { return result::Result(T, Errno); };

    pub const stat := fn(): R(Stat) {
        var out: Stat = undefined;
        const rc := raw_stat(^mut out);
        return if (rc < 0) .{ .err = rc } else .{ .ok = out };
    };
)"};

constexpr std::string_view MID{R"(
    import "result.gh" as result;
    pub constexpr Result := result::Result;
    pub import "leaf.gh" as leaf;
)"};

[[nodiscard]] auto chain_files() {
    return helpers::make_vector<mock_file>(mock_file{"result.gh", RESULT, "result"},
                                           mock_file{"leaf.gh", LEAF, "leaf"},
                                           mock_file{"mid.gh", MID, "mid"});
}

} // namespace

TEST_CASE("E2E: importing a re-exported extern struct with a fixed-array field does not crash") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "mid.gh" as mid;

            pub const main := fn(): i32 {
                return 42;
            };
        )",
        chain_files())};

    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a re-exported extern struct's fixed-array field is usable through the chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "mid.gh" as mid;

            pub const main := fn(): i32 {
                const s: mid::leaf::Stat = .{
                    .dev = 1, .mode = 7, .nlink = 0, .ino = 0,
                    .atime = 0, .mtime = 0, .ctime = 0,
                    .blocks = 0, .blksize = 0, .flags = 0,
                    .spare = [2]i64{ 20, 22 },
                };
                return s.mode + @as(i32, s.spare[0]) + @as(i32, s.spare[1]) - 7;
            };
        )",
        chain_files())};

    CHECK(exit_code == 42);
}

} // namespace ghoti::tests
