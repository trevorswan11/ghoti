#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("E2E: a call may leave trailing defaulted arguments off") {
    CHECK(helpers::compile_and_run(R"(
        const add = fn(a: i32, b: i32 = 10, c: i32 = 100): i32 {
            return a + b + c;
        };

        pub const main = fn(): i32 {
            if (add(1) != 111) { return 1; }
            if (add(1, 2) != 103) { return 2; }
            if (add(1, 2, 3) != 6) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("E2E: a default may read the parameters before it, including other defaults") {
    CHECK(helpers::compile_and_run(R"(
        const area = fn(w: i32, h: i32 = w, scale: i32 = h * 2): i32 {
            return w * h * scale;
        };

        pub const main = fn(): i32 {
            return area(2) + area(2, 3) + area(1, 1, 1); // 16 + 36 + 1
        };
    )") == 53);
}

TEST_CASE("E2E: a default is evaluated again by every call that leaves it off") {
    CHECK(helpers::compile_and_run(R"(
        let mut counter: i32 = 0;
        const next = fn(): i32 {
            counter += 1;
            return counter;
        };
        const take = fn(n: i32 = next()): i32 { return n; };

        pub const main = fn(): i32 {
            let a = take();
            let b = take();
            let c = take(40);
            return a * 100 + b * 10 + c; // 1, 2, 40
        };
    )") == 160);
}

TEST_CASE("E2E: methods, static members, and explicit `self` calls see defaults") {
    CHECK(helpers::compile_and_run(R"(
        const Counter = struct {
            value: i32,

            pub const bump = fn(&mut self, by: i32 = 1): void {
                self.value += by;
            };
            pub const get = fn(&self, offset: i32 = 0): i32 {
                return self.value + offset;
            };
            pub const make = fn(start: i32 = 5): Counter {
                return .{ .value = start };
            };
        };

        pub const main = fn(): i32 {
            let mut c = Counter.make();
            c.bump();
            c.bump(3);
            Counter.bump(&mut c);
            return Counter.get(&c) + c.get(100); // 10 + 110
        };
    )") == 120);
}

TEST_CASE("E2E: a type parameter may have a default") {
    CHECK(helpers::compile_and_run(R"(
        const width = fn(x: i32, T: type = i64): usize {
            _ = x;
            return @sizeOf(T);
        };

        pub const main = fn(): i32 {
            return @intCast(i32, width(1) * 10 + width(1, u16)); // 80 + 2
        };
    )") == 82);
}

TEST_CASE("E2E: a generic function's defaults follow its type parameter") {
    CHECK(helpers::compile_and_run(R"(
        const scaled = fn(T: type, x: T, by: T = 3): T {
            return x * by;
        };

        pub const main = fn(): i32 {
            const a: i64 = scaled(i64, 4);
            const b: u8 = scaled(u8, 2, 5);
            return @intCast(i32, a) + @intCast(i32, b); // 12 + 10
        };
    )") == 22);
}

TEST_CASE("E2E: defaults fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const add = fn(a: i32, b: i32 = a + 1): i32 { return a + b; };
        const FOLDED = add(20);

        pub const main = fn(): i32 {
            comptime {
                @assert(add(1) == 3);
            }
            return FOLDED; // 41
        };
    )") == 41);
}

TEST_CASE("E2E: a local function, a void function, and a discarded parameter take defaults") {
    CHECK(helpers::compile_and_run(R"(
        let mut total: i32 = 0;
        const record = fn(n: i32 = 7): void { total += n; };
        const ignore = fn(a: i32, _: i32 = 9): i32 { return a; };

        pub const main = fn(): i32 {
            const twice = fn(x: i32, times: i32 = 2): i32 { return x * times; };
            record();
            record(3);
            return total + twice(4) + ignore(1); // 10 + 8 + 1
        };
    )") == 19);
}

TEST_CASE("E2E: a parameter pack after defaults takes only the arguments past them") {
    CHECK(helpers::compile_and_run(R"(
        const count = fn(base: i32 = 100, rest...): i32 {
            return base + @intCast(i32, rest.len);
        };

        pub const main = fn(): i32 {
            return count() - 100 + count(5) + count(1, 2, 3); // 0 + 5 + 3
        };
    )") == 8);
}

TEST_CASE("E2E: defaults reach a call across modules") {
    CHECK(helpers::compile_and_run(R"(
            import "geo.gh" as geo;

            pub const main = fn(): i32 {
                return geo.scale(3) + geo.scale(3, 4); // 6 + 12
            };
        )",
                                   {
                                       helpers::mock_file{"geo.gh",
                                                          R"(
                pub const scale = fn(x: i32, by: i32 = 2): i32 { return x * by; };
            )",
                                                          "geo"},
                                   }) == 18);
}

TEST_CASE("E2E: a `const` alias of a function keeps its defaults") {
    CHECK(helpers::compile_and_run(R"(
            import "geo.gh" as geo;

            const double = geo.scale;
            const twice = double;

            pub const main = fn(): i32 {
                return double(3) + twice(4) + geo.times(5); // 6 + 8 + 10
            };
        )",
                                   {
                                       helpers::mock_file{"geo.gh",
                                                          R"(
                pub const scale = fn(x: i32, by: i32 = 2): i32 { return x * by; };
                pub const times = scale;
            )",
                                                          "geo"},
                                   }) == 24);
}

TEST_CASE("A function value leaves its declaration's defaults behind") {
    helpers::expect_compile_error(R"(
        const add = fn(a: i32, b: i32 = 1): i32 { return a + b; };

        pub const main = fn(): i32 {
            let f = add;
            return f(1);
        };
    )");
}

TEST_CASE("Too few arguments for the parameters without defaults is still an error") {
    helpers::expect_compile_error(R"(
        const add = fn(a: i32, b: i32, c: i32 = 1): i32 { return a + b + c; };
        pub const main = fn(): i32 { return add(1); };
    )");
}

TEST_CASE("A default of the wrong type is an error even with no call leaving it off") {
    helpers::expect_compile_error(R"(
        const add = fn(a: i32, b: i32 = true): i32 { return a + b; };
        pub const main = fn(): i32 { return add(1, 2); };
    )");
}

} // namespace ghoti::tests
