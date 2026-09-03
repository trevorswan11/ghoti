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

extern const __ghoti_test_take_skipped: fn(): bool;

pub weak const test_runner := fn(args: [][:0]u8, tests: []Test): i32 {
    _ = args;
    var failures: i32 = 0;
    var skipped: i32 = 0;
    for (tests) |entry| {
        const passed := entry.func();
        if (__ghoti_test_take_skipped()) {
            skipped = skipped + 1;
        } else if (!passed) {
            failures = failures + 1;
        }
    }
    _ = skipped;
    return failures;
};

pub weak const panic_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): noreturn {
    _ = msg;
    _ = file;
    _ = line;
    _ = column;
    @trap();
};

pub weak const assert_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): void {
    _ = msg;
    _ = file;
    _ = line;
    _ = column;
    @trap();
};

pub weak const expect_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): void {
    _ = msg;
    _ = file;
    _ = line;
    _ = column;
};

pub weak const require_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): void {
    _ = msg;
    _ = file;
    _ = line;
    _ = column;
};

pub weak const skip_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): void {
    _ = msg;
    _ = file;
    _ = line;
    _ = column;
};
)"
