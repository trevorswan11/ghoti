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

TEST_CASE("E2E: a cross-module trait-impl method calls a sibling through a non-`self` receiver") {
    constexpr std::string_view iface_gh{R"(
        pub const W := interface {
            pub const put := fn(&mut self, n: i32): i32;
            pub const putN := fn(&mut self, n: i32): i32 { return self.put(n); };
        };
    )"};
    constexpr std::string_view dev_gh{R"(
        import "iface.gh" as iface;
        pub const Dev := struct { acc: i32 };
        impl iface::W for Dev {
            pub const put := fn(&mut this, n: i32): i32 { this.acc += n; return this.acc; };
            pub const putN := fn(&mut this, n: i32): i32 { return this.put(n) + this.put(n); };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "dev.gh" as d;
            pub const main := fn(): i32 {
                var x := d::Dev{ .acc = 0 };
                return x.putN(10);
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

TEST_CASE("E2E: a cross-module interface default method the impl does not override is inherited") {
    constexpr std::string_view adder_gh{R"(
        pub const Adder := interface {
            pub const step := fn(&self): i32;
            pub const stepThrice := fn(&self): i32 {
                return self.step() + self.step() + self.step();
            };
        };
    )"};
    constexpr std::string_view one_gh{R"(
        import "adder.gh" as adder;
        pub const One := struct { by: i32 };
        impl adder::Adder for One {
            pub const step := fn(&self): i32 { return self.by; };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "one.gh" as m;
            pub const main := fn(): i32 {
                var o := m::One{ .by = 14 };
                return o.stepThrice();
            };
        )",
        {
            mock_file{"adder.gh", adder_gh, "adder"},
            mock_file{"one.gh", one_gh, "one"},
        })};
    CHECK(exit_code == 42);
}

TEST_CASE(
    "E2E: an inherited cross-module default method re-types `self.req()?` and an assoc type") {
    constexpr std::string_view res_gh{
        R"(pub const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };)"};
    constexpr std::string_view writer_gh{R"(
        import "res.gh" as res;
        pub const Writer := interface {
            Error: type;
            pub const write := fn(&mut self, bytes: []u8): res::Result(usize, Error);
            pub const writeAll := fn(&mut self, bytes: []u8): res::Result(void, Error) {
                var off: usize = 0;
                loop {
                    if (off == bytes.len) { break; }
                    const n := self.write(bytes[off..])?;
                    off += n;
                };
                return .{ .ok = {} };
            };
        };
    )"};
    constexpr std::string_view sink_gh{R"(
        import "writer.gh" as writer;
        import "res.gh" as res;
        pub const Sink := struct { pub total: usize };
        impl writer::Writer for Sink {
            using Error = u8;
            pub const write := fn(&mut self, bytes: []u8): res::Result(usize, Error) {
                self.total += bytes.len;
                return .{ .ok = bytes.len };
            };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sink.gh" as m;
            pub const main := fn(): i32 {
                var s := m::Sink{ .total = 0 };
                const buf := [3uz]u8{ 1, 2, 3 };
                _ = s.writeAll(buf);
                return @as(i32, s.total);
            };
        )",
        {
            mock_file{"res.gh", res_gh, "res"},
            mock_file{"writer.gh", writer_gh, "writer"},
            mock_file{"sink.gh", sink_gh, "sink"},
        })};
    CHECK(exit_code == 3);
}

TEST_CASE("E2E: one inherited cross-module default method calls another through `self`") {
    constexpr std::string_view res_gh{
        R"(pub const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };)"};
    constexpr std::string_view writer_gh{R"(
        import "res.gh" as res;
        pub const Writer := interface {
            Error: type;
            pub const write := fn(&mut self, bytes: []u8): res::Result(usize, Error);
            pub const writeAll := fn(&mut self, bytes: []u8): res::Result(void, Error) {
                var off: usize = 0;
                loop {
                    if (off == bytes.len) { break; }
                    off += self.write(bytes[off..])?;
                };
                return .{ .ok = {} };
            };
            pub const writeByte := fn(&mut self, b: u8): res::Result(void, Error) {
                const one := [1uz]u8{ b };
                return self.writeAll(one);
            };
        };
    )"};
    constexpr std::string_view sink_gh{R"(
        import "writer.gh" as writer;
        import "res.gh" as res;
        pub const Sink := struct { pub total: usize };
        impl writer::Writer for Sink {
            using Error = u8;
            pub const write := fn(&mut self, bytes: []u8): res::Result(usize, Error) {
                self.total += bytes.len;
                return .{ .ok = bytes.len };
            };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sink.gh" as m;
            pub const main := fn(): i32 {
                var s := m::Sink{ .total = 0 };
                _ = s.writeByte('x');
                _ = s.writeByte('y');
                return @as(i32, s.total);
            };
        )",
        {
            mock_file{"res.gh", res_gh, "res"},
            mock_file{"writer.gh", writer_gh, "writer"},
            mock_file{"sink.gh", sink_gh, "sink"},
        })};
    CHECK(exit_code == 2);
}

TEST_CASE("E2E: a second impl of the same interface still inherits its default methods") {
    constexpr std::string_view res_gh{
        R"(pub const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };)"};
    constexpr std::string_view reader_gh{R"(
        import "res.gh" as res;
        pub const Reader := interface {
            Error: type;
            pub const read := fn(&mut self, buf: []mut u8): res::Result(usize, Error);
            pub const readAll := fn(&mut self, buf: []mut u8): res::Result(usize, Error) {
                var i: usize = 0;
                loop {
                    if (i == buf.len) { break; }
                    const n := self.read(buf[i..])?;
                    if (n == 0) { break; }
                    i += n;
                };
                return .{ .ok = i };
            };
        };
    )"};

    constexpr std::string_view early_gh{R"(
        import "reader.gh" as reader;
        import "res.gh" as res;
        pub const Early := struct { pub data: []mut u8, pub pos: usize = 0 };
        impl reader::Reader for Early {
            using Error = u8;
            pub const read := fn(&mut self, buf: []mut u8): res::Result(usize, Error) {
                const rem := self.data.len - self.pos;
                const n := if (buf.len < rem) buf.len else rem;
                self.pos += n;
                return .{ .ok = n };
            };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "early.gh" as early;
            import "reader.gh" as reader;
            import "res.gh" as res;

            const Late := struct { pub data: []mut u8, pub pos: usize = 0 };
            impl reader::Reader for Late {
                using Error = u8;
                pub const read := fn(&mut self, buf: []mut u8): res::Result(usize, Error) {
                    const rem := self.data.len - self.pos;
                    const n := if (buf.len < rem) buf.len else rem;
                    self.pos += n;
                    return .{ .ok = n };
                };
            }

            pub const main := fn(): i32 {
                var eb: [6]mut u8 = undefined;
                var e := early::Early{ .data = eb };
                var lb: [6]mut u8 = undefined;
                var l := Late{ .data = lb };
                var out: [4]mut u8 = undefined;
                const en := match (e.readAll(out[..])) { .ok => |n| n, .err => 0uz };
                const ln := match (l.readAll(out[..])) { .ok => |n| n, .err => 0uz };
                return @as(i32, en + ln);
            };
        )",
        {
            mock_file{"res.gh", res_gh, "res"},
            mock_file{"reader.gh", reader_gh, "reader"},
            mock_file{"early.gh", early_gh, "early"},
        })};
    CHECK(exit_code == 8);
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
