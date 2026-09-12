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

/// The kind of a type, as returned by `@typeInfo(T).kind` / `@tagName`.
pub const TypeKind := enum {
    int, float, @"bool", @"void", @"noreturn", @"opaque", @"type",
    pointer, reference, slice, array,
    @"struct", @"union", @"enum", function, internal,
};

/// Exact set of calling conventions ghoti supports, matching `ast::calling_convention` 1:1.
pub const CallConv := enum { c, sysv, win64, stdcall, fastcall, aapcs };

pub const IntInfo := struct {
    pub bits: u16,
    pub signed: bool,
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

pub const FieldInfo := struct {
    pub name: []u8,
    pub type_: type,
    pub has_default: bool,
};

pub const StructInfo := struct {
    pub fields: []FieldInfo,
    pub is_extern: bool,
    pub is_packed: bool,
    pub backing_bits: u32,
};

// A union field never has a default value at all (`ast::union_expr::field` carries no
// `default_value` syntax) - a leaner shape than `FieldInfo` instead of a `has_default` that can
// only ever read `false`.
pub const UnionFieldInfo := struct {
    pub name: []u8,
    pub type_: type,
};

pub const UnionInfo := struct {
    pub fields: []UnionFieldInfo,
    pub is_extern: bool,
    pub is_packed: bool,
    pub tagged: bool,
};

pub const EnumField := struct {
    pub name: []u8,
    pub value: i64,
};

pub const EnumInfo := struct {
    pub tag_type: type,
    pub fields: []EnumField,
    pub exhaustive: bool,
};

pub const FnInfo := struct {
    pub params: []type,
    pub return_type: type,
    pub variadic: bool,
    pub has_self: bool,
    pub @"callconv": CallConv,
};

// A payload-less union arm's type. Deliberately not spelled `void` here: `TypeInfo` below has
// arms literally *named* `bool`/`void`/etc. (mirroring `TypeKind`), and resolving a sibling arm's
// bare `void` type from *inside* that union's own body found the union's own `void`-named arm (a
// value symbol) ahead of the builtin `void` type - a real, narrow scoping bug where an inner
// same-named member shadows an outer keyword-named type even in a position where only a type can
// go. A `using void := void;`-style alias dodges the name collision but does not dodge the
// deeper issue: LLVM's `void` has no size at all, and a codegen path that reaches it through an
// alias asserts trying to size it (`getTypeInfo` on an unsized type) instead of routing around
// it the way a bare `void` union arm already does elsewhere in this compiler. A zero-sized empty
// struct sidesteps both problems - no colliding name, and a real (if empty) sized layout.
const NoPayload := struct {};

/// The keystone reflection value: `@typeInfo(T)` returns one of these, tagged by `TypeKind`.
pub const TypeInfo := union {
    int: IntInfo,
    float: FloatInfo,
    @"bool": NoPayload,
    @"void": NoPayload,
    @"noreturn": NoPayload,
    @"opaque": NoPayload,
    @"type": NoPayload,
    internal: NoPayload,
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
