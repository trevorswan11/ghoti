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
    pub loc: SourceLocation,
    pub func: extern fn(): bool,
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
    pub const from_residual := fn(r: From): @This();
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

pub weak const panic_handler := fn(_: []u8, _: SourceLocation): noreturn {
    @trap();
};

pub weak const assert_handler := fn(_: []u8, _: SourceLocation): void {
    @trap();
};

pub weak const expect_handler := fn(_: []u8, _: SourceLocation): void {};

pub weak const require_handler := fn(_: []u8, _: SourceLocation): void {};

pub weak const skip_handler := fn(_: []u8, _: SourceLocation): void {};

/// Exact set of calling conventions ghoti supports, matching `ast::calling_convention` 1:1.
pub const CallConv := enum { c, sysv, win64, stdcall, fastcall, aapcs };

pub const Signedness := enum : u1 { signed, unsigned };

pub const IntInfo := struct {
    pub bits: u16,
    pub signedness: Signedness,
};

pub const FloatInfo := struct {
    pub bits: u16,
};

pub const PointerInfo := struct {
    pub child: type,
    pub is_mut: bool,
    pub is_volatile: bool,
};

pub const SliceInfo := struct {
    pub child: type,
    pub sentinel: bool,
    pub is_mut: bool,
    pub is_volatile: bool,
};

pub const ArrayInfo := struct {
    pub child: type,
    pub len: usize,
    pub sentinel: bool,
    pub is_mut: bool,
    pub is_volatile: bool,
};

pub const StructFieldInfo := struct {
    pub name: []u8,
    pub @"type": type,
    /// The field's default value, cast through `^opaque`, or `nullptr` for none.
    pub default_value: ^opaque = nullptr,
};

pub const StructInfo := struct {
    pub fields: []StructFieldInfo,
    pub is_extern: bool,
    pub is_packed: bool,
    pub backing_bits: u32,
};

pub const UnionFieldInfo := struct {
    pub name: []u8,
    pub @"type": type,
};

pub const UnionInfo := struct {
    pub fields: []UnionFieldInfo,
    pub is_extern: bool,
    pub is_packed: bool,
    pub tagged: bool,
};

pub const EnumFieldInfo := struct {
    pub name: []u8,
    pub value: constexpr_int,
};

pub const EnumInfo := struct {
    pub tag_type: type,
    pub fields: []EnumFieldInfo,
    pub exhaustive: bool,
};

pub const FnInfo := struct {
    pub params: []type,
    pub return_type: type,
    pub variadic: bool,
    pub has_self: bool,
    pub @"callconv": CallConv,
};

/// The keystone reflection value: `@typeInfo(T)` returns one of these.
pub const TypeInfo := union {
    int: IntInfo,
    float: FloatInfo,
    @"constexpr_int": void,
    @"constexpr_float": void,
    @"bool": void,
    @"void": void,
    @"noreturn": void,
    @"opaque": void,
    @"type": void,
    internal: void,
    pointer: PointerInfo,
    reference: PointerInfo,
    slice: SliceInfo,
    array: ArrayInfo,
    @"struct": StructInfo,
    @"union": UnionInfo,
    @"enum": EnumInfo,
    function: FnInfo,
};
)"
