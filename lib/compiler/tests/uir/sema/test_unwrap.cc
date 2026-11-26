#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

// A small optional and result, each unwrappable and rewrappable
constexpr std::string_view TYPES{R"(
    const Opt = union(enum) {
        some: i32,
        none: void,
    };

    impl builtin.Unwrappable for Opt {
        const Output = i32;
        const Residual = void;
        pub const branch = fn(&mut? self): builtin.Flow(&mut? i32, void) {
            return match (self) {
                .some => |&mut? v| .{ .continue = v },
                .none => .{ .break = {} },
            };
        };
    }

    impl builtin.Rewrappable for Opt {
        const From = void;
        pub const from_residual = fn(_: void): @This() {
            return .{ .none = {} };
        };
    }

    const Res = union(enum) {
        ok: i32,
        err: u8,
    };

    impl builtin.Unwrappable for Res {
        const Output = i32;
        const Residual = u8;
        pub const branch = fn(&mut? self): builtin.Flow(&mut? i32, u8) {
            return match (self) {
                .ok => |&mut? v| .{ .continue = v },
                .err => |e| .{ .break = e },
            };
        };
    }

    impl builtin.Rewrappable for Res {
        const From = u8;
        pub const from_residual = fn(e: u8): @This() {
            return .{ .err = e };
        };
    }
)"};

[[nodiscard]] auto with_types(std::string_view body) -> std::string {
    return std::string{TYPES} + std::string{body};
}

} // namespace

TEST_CASE("A match through a reference captures by reference", "[uir]") {
    const auto diff{diff_source(R"(
        const U = union(enum) { a: i32, b: void };
        pub const bump = fn(u: &mut U): void {
            match (u) {
                .a => |&mut v| { v += 1; },
                .b => {},
            }
        };
        pub const read = fn(u: &U): i32 {
            return match (u) {
                .a => |&v| v,
                .b => 0,
            };
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A mutable capture of an immutable value is rejected", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const U = union(enum) { a: i32, b: void };
        pub const f = fn(u: &U): void {
            match (u) {
                .a => |&mut v| { v += 1; },
                .b => {},
            }
        };
    )")};
    CHECK(run.codes() == std::vector{error::ASSIGNMENT_TO_CONST});
    CHECK(run.messages().front() == "Cannot capture an immutable value by mutable reference");
}

TEST_CASE("`?` and `!` lower to branch, a tag test and the residual path", "[uir]") {
    const auto diff{diff_source(with_types(R"(
        pub const plus_one = fn(o: Opt): Opt {
            let v = o?;
            return .{ .some = v + 1 };
        };
        pub const force = fn(o: Opt): i32 { return o!; };
        pub const checked = fn(r: Res): Res {
            let v = r?;
            return .{ .ok = v * 2 };
        };
        pub const force_res = fn(r: Res): i32 { return r!; };
    )"))};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`?` runs the defers above it before returning", "[uir]") {
    const auto diff{diff_source(with_types(R"(
        pub let mut count: i32 = 0;
        pub const f = fn(r: Res): Res {
            defer count += 1;
            let v = r?;
            return .{ .ok = v };
        };
    )"))};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An unwrapping `if` captures the payload and the residual", "[uir]") {
    const auto diff{diff_source(with_types(R"(
        pub const value_or = fn(o: Opt, fallback: i32): i32 {
            if (o) |v| { return v; }
            return fallback;
        };
        pub const code = fn(r: Res): i32 {
            if (r) |v| { return v + 1; } else |e| { return @intCast(e); }
        };
        pub const bump = fn(r: &mut Res): void {
            if (r) |&mut v| { v += 1; }
        };
    )"))};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An unwrapping `while` runs branch on every iteration", "[uir]") {
    const auto diff{diff_source(with_types(R"(
        const Counter = struct { left: i32 };
        const next = fn(c: &mut Counter): Res {
            if (c.left == 0) { return .{ .err = 7 }; }
            c.left -= 1;
            return .{ .ok = c.left };
        };
        pub const total = fn(n: i32): i32 {
            let mut c: Counter = .{ .left = n };
            let mut sum: i32 = 0;
            while (next(&mut c)) |v| : (sum += v) {
                sum += 1;
            } else |e| {
                sum += @intCast(e);
            }
            return sum;
        };
    )"))};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A reference a method returns is read before the other operand", "[uir]") {
    const auto diff{diff_source(R"(
        const P = struct { a: i32, pub const get = fn(&self): &i32 { return &self.a; }; };
        const g = fn(): i32 { return 3; };
        pub const main = fn(): i32 { let p: P = .{ .a = 2 }; return p.get() + g(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An `else` capture of a residual that carries nothing is reported", "[uir]") {
    const auto run{helpers::uir_rejected(with_types(R"(
        pub const f = fn(o: Opt): i32 {
            return if (o) |v| v else |e| 0;
        };
    )"))};
    CHECK(run.codes() == std::vector{error::ILLEGAL_UNWRAP_CAPTURE});
    CHECK(run.messages().front() ==
          "'Opt' carries nothing when it doesn't unwrap, so its `else` has no value to capture");
}

TEST_CASE("`?` on a type that isn't unwrappable is reported", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const U = union(enum) { a: i32, b: void };
        pub const f = fn(u: U): i32 { return u!; };
    )")};
    CHECK(run.codes() == std::vector{error::UNWRAP_ON_NON_RESULT});
    CHECK(run.messages().front() ==
          "the postfix '!' operator expects a type implementing 'builtin.Unwrappable'; 'U' does "
          "not implement it; add 'impl builtin.Unwrappable for U'");
}

TEST_CASE("`?` in a function whose return type can't take the residual is reported", "[uir]") {
    const auto run{helpers::uir_rejected(with_types(R"(
        pub const f = fn(o: Opt): i32 {
            let v = o?;
            return v;
        };
    )"))};
    CHECK(run.codes() == std::vector{error::UNWRAP_RETURN_TYPE_MISMATCH});
    CHECK(run.messages().front() ==
          "the '?' operator propagates a 'Opt' residual ('void') but 'i32' does not implement "
          "'builtin.Rewrappable'");
}

TEST_CASE("A `dyn` of an interface with a defaulted associated type", "[uir]") {
    const auto diff{diff_source(R"(
        const Sink = interface {
            Error: type = u8;
            pub const put = fn(&mut self, b: u8): i32;
            pub const code = fn(&mut self, e: Error): i32 { return self.put(e) + 1; };
        };
        const Count = struct { n: i32 };
        impl Sink for Count {
            pub const put = fn(&mut self, b: u8): i32 {
                self.n += 1;
                return @intCast(b);
            };
        }
        const twice = fn(s: &mut dyn Sink): i32 { return s.code(3) + s.put(4); };
        pub const main = fn(): i32 {
            let mut c: Count = .{ .n = 0 };
            return twice(&mut c) + c.n;
        };
    )")};
    INFO(diff.detail);
    // The old GIR types the default method's `e: Error` as `type`, a recorded legacy bug
    CHECK(diff.status == helpers::uir_diff_status::LEGACY_BUG);
}

TEST_CASE("A `dyn` names its associated types' defaults", "[uir]") {
    const auto diff{diff_source(R"(
        const Sink = interface {
            Error: type = u8;
            pub const put = fn(&mut self, b: u8): i32;
            pub const twice = fn(&mut self, b: u8): i32 { return self.put(b) + self.put(b); };
        };
        const Count = struct { n: i32 };
        impl Sink for Count {
            pub const put = fn(&mut self, b: u8): i32 {
                self.n += 1;
                return @intCast(b);
            };
        }
        const run = fn(s: &mut dyn Sink): i32 { return s.twice(3); };
        pub const main = fn(): i32 {
            let mut c: Count = .{ .n = 0 };
            return run(&mut c) + c.n;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A match on a call's aggregate result takes it apart in a copy", "[uir]") {
    const auto diff{diff_source(with_types(R"(
        const make = fn(x: i32): Res { if (x < 0) { return .{ .err = 1 }; } return .{ .ok = x }; };
        pub const code = fn(x: i32): i32 {
            return match (make(x)) {
                .ok => |v| v,
                .err => x - 1,
            };
        };
    )"))};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Unwrapping `if`, `while`, `?` and errdefer run at compile time", "[uir]") {
    const auto run{helpers::uir_accepted(with_types(R"(
        const pick = fn(o: Opt): i32 { if (o) |v| { return v; } else { return -1; } };
        const code = fn(r: Res): i32 { return if (r) |v| v else |e| @as(i32, e) + 100; };
        const Counter = struct { left: i32 };
        const next = fn(c: &mut Counter): Res {
            if (c.left == 0) { return .{ .err = 7 }; }
            c.left -= 1;
            return .{ .ok = c.left };
        };
        const total = fn(n: i32): i32 {
            let mut c: Counter = .{ .left = n };
            let mut sum: i32 = 0;
            while (next(&mut c)) |v| { sum += v; } else |e| { sum += @as(i32, e) * 10; }
            return sum;
        };
        const plus = fn(o: Opt): Opt {
            let v = o?;
            return .{ .some = v + 1 };
        };
        const or_zero = fn(o: Opt): i32 { return if (o) |v| v else 0; };
        const bump = fn(): i32 {
            let mut o: Opt = .{ .some = 1 };
            if (o) |&mut v| { v += 4; }
            return or_zero(o);
        };
        const logged = fn(r: Res, log: &mut i32): Res {
            defer log += 1;
            errdefer log += 100;
            let v = r?;
            return .{ .ok = v };
        };
        const run_log = fn(r: Res): i32 {
            let mut log: i32 = 0;
            _ = logged(r, &mut log);
            return log;
        };
        const some = pick(.{ .some = 5 });
        const none = pick(.{ .none = {} });
        const residual = code(.{ .err = 3 });
        const looped = total(3);
        const propagated = or_zero(plus(.{ .some = 1 }));
        const stopped = or_zero(plus(.{ .none = {} }));
        const written = bump();
        const failed = run_log(.{ .err = 1 });
        const passed = run_log(.{ .ok = 1 });
    )"))};
    CHECK(run.int_value("some") == 5);
    CHECK(run.int_value("none") == -1);
    CHECK(run.int_value("residual") == 103);
    CHECK(run.int_value("looped") == 73);
    CHECK(run.int_value("propagated") == 2);
    CHECK(run.int_value("stopped") == 0);
    CHECK(run.int_value("written") == 5);
    CHECK(run.int_value("failed") == 101);
    CHECK(run.int_value("passed") == 1);
}

TEST_CASE("A `!` that fails at compile time is reported", "[uir]") {
    const auto run{helpers::uir_rejected(with_types(R"(
        const get = fn(o: Opt): i32 { return o!; };
        const unwrapped = get(.{ .none = {} });
    )"))};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK(run.messages().front() == "'!' unwrapped an empty optional");
}

} // namespace ghoti::tests
