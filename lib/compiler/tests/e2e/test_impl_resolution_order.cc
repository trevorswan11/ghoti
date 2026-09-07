#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("E2E: an `impl` block on a struct whose method returns `Ctor(Self)` compiles") {
    const auto exit_code{helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        const Err := enum : i32 { bad, _ };

        const File := struct {
            pub handle: i32,
            pub const open := fn(h: i32): Result(File, Err) {
                return .{ .ok = .{ .handle = h } };
            };
        };
        impl File {}

        pub const main := fn(): i32 {
            const r := File.open(7);
            return match (r) { .ok => |f| f.handle, .err => 0 };
        };
    )")};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: an `impl` target's method param names a not-yet-resolved sibling struct") {
    const auto exit_code{helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        const Err := enum : i32 { bad, _ };

        const Mode := enum : u8 { read, write };
        const Flags := struct { pub m: Mode = .read, pub create: bool = false };

        const File := struct {
            pub handle: i32,
            pub const open := fn(flags: Flags): Result(File, Err) {
                if (flags.create) { return .{ .err = .bad }; }
                return .{ .ok = .{ .handle = 3 } };
            };
        };
        impl File {}

        pub const main := fn(): i32 {
            const f: Flags = .{ .m = .write, .create = false };
            return match (File.open(f)) { .ok => |x| x.handle, .err => 0 };
        };
    )")};
    CHECK(exit_code == 3);
}

TEST_CASE("E2E: a cross-module `using` alias in an `impl` target's method signature resolves") {
    constexpr std::string_view result_gh{
        R"(pub const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };)"};
    constexpr std::string_view err_gh{R"(pub const Error := enum : i32 { bad, worse, _ };)"};
    constexpr std::string_view file_gh{R"(
        import "result.gh" as result;
        import "err.gh" as error;
        using Result = result::Result;

        pub const File := struct {
            pub handle: i32,
            pub const open := fn(h: i32): Result(File, error::Error) {
                return .{ .ok = .{ .handle = h } };
            };
        };
        impl File {}
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "file.gh" as f;
            pub const main := fn(): i32 {
                return match (f::File.open(9)) { .ok => |x| x.handle, .err => 0 };
            };
        )",
        {
            mock_file{"result.gh", result_gh, "result"},
            mock_file{"err.gh", err_gh, "err"},
            mock_file{"file.gh", file_gh, "file"},
        })};
    CHECK(exit_code == 9);
}

TEST_CASE("E2E: a cross-module trait-impl method calls a sibling method through `self`") {
    constexpr std::string_view iface_gh{R"(
        pub const W := interface {
            pub const put := fn(&mut self, n: i32): i32;
            pub const putN := fn(&mut self, n: i32, k: i32): i32 { return self.put(n); };
        };
    )"};
    constexpr std::string_view dev_gh{R"(
        import "iface.gh" as iface;
        pub const Dev := struct { acc: i32 };
        impl iface::W for Dev {
            pub const put := fn(&mut self, n: i32): i32 { self.acc += n; return self.acc; };
            pub const putN := fn(&mut self, n: i32, k: i32): i32 {
                _ = k;
                return self.put(n) + self.put(n);
            };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "dev.gh" as d;
            pub const main := fn(): i32 {
                var x := d::Dev{ .acc = 0 };
                return x.putN(10, 3);
            };
        )",
        {
            mock_file{"iface.gh", iface_gh, "iface"},
            mock_file{"dev.gh", dev_gh, "dev"},
        })};
    CHECK(exit_code == 30);
}

TEST_CASE("E2E: a static enum method reached cross-module via a `using` alias is a direct call") {
    constexpr std::string_view err_gh{R"(
        pub const Error := enum : i32 {
            not_found, other, _,
            pub const fromCode := fn(c: i32): @this() {
                return if (c == 0i32) .not_found else .other;
            };
        };
    )"};
    constexpr std::string_view wrap_gh{R"(
        import "err.gh" as error;
        using Error = error::Error;
        pub const classify := fn(c: i32): Error { return Error.fromCode(c); };
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "wrap.gh" as wrap;
            pub const main := fn(): i32 {
                return match (wrap::classify(0i32)) { .not_found => 5, _ => 1 };
            };
        )",
        {
            mock_file{"err.gh", err_gh, "err"},
            mock_file{"wrap.gh", wrap_gh, "wrap"},
        })};
    CHECK(exit_code == 5);
}

TEST_CASE("E2E: a re-exported function called as `mod::fn(...)` links to its real owner") {
    constexpr std::string_view backend_gh{R"(pub const dup := fn(n: i32): i32 { return n + n; };)"};
    constexpr std::string_view os_gh{R"(
        import "backend.gh" as backend;
        pub const dup := backend::dup;
    )"};
    constexpr std::string_view other_gh{R"(pub const dup := fn(): i32 { return 999; };)"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "os.gh" as os;
            import "other.gh" as other;
            pub const main := fn(): i32 {
                _ = other::dup;
                return os::dup(21);
            };
        )",
        {
            mock_file{"backend.gh", backend_gh, "backend"},
            mock_file{"os.gh", os_gh, "os"},
            mock_file{"other.gh", other_gh, "other"},
        })};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: two instantiations of a nested `fn(...): type` constructor stay distinct") {
    const auto exit_code{helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        const Errno := enum : i32 { bad, _ };
        const R := fn(T: type): type { return Result(T, Errno); };

        const Stat := struct { n: u64, pub const size := fn(&self): u64 { return self.n; }; };

        const get_num := fn(): R(usize) { return .{ .ok = 7uz }; };
        const get_stat := fn(): R(Stat) { return .{ .ok = .{ .n = 35u64 } }; };

        pub const main := fn(): i32 {
            const a := match (get_num()) { .ok => |n| n, .err => 0uz };
            const b := match (get_stat()) { .ok => |s| s.size(), .err => 0u64 };
            return @as(i32, a) + @as(i32, b);
        };
    )")};
    CHECK(exit_code == 42);
}

} // namespace ghoti::tests
