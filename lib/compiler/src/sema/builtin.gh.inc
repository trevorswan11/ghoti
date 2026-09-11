R"(pub const Os := enum {
    linux,
    macos,
    ios,
    windows,
    freebsd,
    openbsd,
    netbsd,
    dragonfly,
    solaris,
    haiku,
    wasi,
    emscripten,
    uefi,
    freestanding,
    _,
};

pub const Arch := enum {
    x86_64,
    x86,
    aarch64,
    arm,
    thumb,
    riscv64,
    riscv32,
    wasm32,
    wasm64,
    powerpc64,
    powerpc,
    mips64,
    mips,
    s390x,
    loongarch64,
    sparc64,
    _,
};

pub const Abi := enum {
    gnu,
    gnueabi,
    gnueabihf,
    gnux32,
    gnuabi64,
    musl,
    musleabi,
    musleabihf,
    msvc,
    eabi,
    eabihf,
    itanium,
    none,
    _,
};

pub const Family := enum { unix, windows, wasm, other, _ };
pub const Endian := enum { little, big };

pub const MemoryOrder := enum { relaxed, acquire, release, acq_rel, seq_cst };
pub const AtomicRmwOp := enum {
    xchg, add, sub, band, nand, bor, bxor, max, min, umax, umin,
};

pub const SourceLocation := struct {
    pub file: []u8,
    pub line: u32,
    pub column: u32,
};

pub const Test := struct {
    pub name: []u8,
    pub file: []u8,
    pub line: u32,
    pub column: u32,
    pub func: fn(): bool,
};

/// Control flow tagged union for single-match nominal unwrapping (`?` and `!`).
pub const Flow := fn(C: type, R: type): type {
    return union {
        @"continue": C, /// Holds the success payload
        @"break": R, /// Holds the early-return residual
    };
};

/// Interface implemented by types supporting postfix `?` and `!` unwrapping.
/// Maps the instance into a `Flow(Output, Residual)` union via `branch(self)`.
pub const Unwrappable := interface {
    Output: type;
    Residual: type;
    pub const branch := fn(self): Flow(Output, Residual);
};

/// Interface implemented by enclosing function return types to construct an early-return
/// value from an unwrap expression's residual payload during `?` propagation.
pub const Rewrappable := interface {
    From: type;
    pub const fromResidual := fn(r: From): @This();
};

extern const __ghoti_test_take_skipped: fn(): bool;

pub weak const test_runner := fn(_: [][:0]u8, tests: []Test): i32 {
    var failures: i32 = 0;
    var skipped: i32 = 0;
    for (tests) |entry| {
        const passed := entry.func();
        if (__ghoti_test_take_skipped()) {
            skipped += 1;
        } else if (!passed) {
            failures += 1;
        }
    }
    _ = skipped;
    return failures;
};

pub weak const panic_handler := fn(_: []u8, _: []u8, _: u32, _: u32): noreturn {
    @trap();
};

pub weak const assert_handler := fn(_: []u8, _: []u8, _: u32, _: u32): void {
    @trap();
};

pub weak const expect_handler := fn(_: []u8, _: []u8, _: u32, _: u32): void {};

pub weak const require_handler := fn(_: []u8, _: []u8, _: u32, _: u32): void {};

pub weak const skip_handler := fn(_: []u8, _: []u8, _: u32, _: u32): void {};
)"
