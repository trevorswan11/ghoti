# v0.1.0
- Initial release

# v0.2.0

## Testing
- `test` blocks now compile and run: `ghoti test` command, per-test functions, and test metadata (`builtin::Test`)
- `@expect` / `@require` assertions and `@skip()` (rejected outside a `test` block)
- Weak `test_runner` / `panic_handler` / `expect_handler` / `require_handler` / `skip_handler` hooks in the `builtin` module for running & failure/skip reporting
    - `test_runner`'s signature is `fn(args: [][:0]u8, tests: []Test): i32`
- `ghoti test <file> -- <args>` (or bare trailing args) forwards `<args>` to the compiled test binary's `argv`
- `ghoti test -o <path>` writes the test binary to `<path>` and keeps it, instead of building to a temp file that is deleted after the run

## Language
- `constexpr` function parameters
- Postfix `?` / `!` unwrap operators for `Result` / `Optional`
    - Requires any tagged union in the shape `union { .ok/.some: ..., .err/.none:... }`
- `if constexpr`: dead branches are no longer resolved or emitted, with per-instantiation pruning
- `match` on a compile-time `type`: dispatches on exact type identity
    - `if constexpr`-style (only the selected arm is checked/emitted)
    - Requires a `_` arm
    - Works per generic `T: type` instantiation
    - Patterns and scrutinees may be primitives, named types, `^T` / `&T`, `[]T` / `[N]T`, and `fn(...)...`
- `[]T`, `[N]T`, `[:0]T`, and bodyless `fn(...)...` are now valid in value position
    - Usable as `const` aliases, function parameter/return types, and passed to `T: type` parameters
- A `T: type` parameter accepts any type-denoting argument, including `^Point`, `[]u8`, and local aliases
- Positional aggregate literals: `Alias{ a, b, c }` and implicit `.{ a, b, c }` initialize an array-type alias by element order
- Compile-time config: `@cfg`, `@cfgValue`, `@compileError`; target facts as `builtin` enums; `@cfg` can gate aggregate fields and nested control flow
- Inline assembly: `asm { ... }` expressions
- Declaration modifiers: `weak`, `naked`, `threadlocal`, and link-name overrides
- `undefined` for uninitialized bindings; required for non-`extern` uninitialized values
- `return` accepts implicit initializers (`return .{ ... }` / `return .variant`) through `if` / `match` / labeled `break`
- Module-scope `var` globals and `var` / `const` static members are first-class: bare-name / `Type.X` / `@this().X` read, address-of, and assignment; member functions usable as `fn` pointers
- Non-exhaustive enums: casting an integer to an enum without `_` is range-checked at runtime and panics on an unlisted value
- Fixed-width integers `i8`, `i16`, `u16` (widen implicitly: `i8`→`i16`→`i32`→`i64`/`isize`, `u8`→`u16`→`u32`→`u64`/`usize`)
- Pointer truthiness: a `^T` is non-null-tested in boolean position
    - `if (ptr)`, `while (ptr)`, `do…while (ptr)`, `!ptr`, and `and` / `or` operands
    - Implicit coercion (`const b: bool = ptr`, passing a pointer to a `bool` parameter) is still rejected
- Explicit `@as(bool, ptr)`; `@as(bool, x)` also accepts an integer (`x != 0`)
- Reference-typed struct and union fields are allowed; they are rejected in `extern` struct / union

## Runtime safety
- Signed `+ - * -x` overflow, integer division / remainder by zero (signed **and** unsigned), `INT_MIN / -1`, and out-of-range shift amounts now panic at runtime
- Dereferencing a null `^T` panics: through `*p`, `p.field`, `p[i]`, and `p[lo..hi]` (reads and writes)
    - `&T` references are exempt (non-null by construction)
- `@addWithOverflow` / `@mulWithOverflow` / `@divTrunc` / ... stay as the unchecked escape hatches; unsigned `+ - *` still wrap
- `--unsafe` build flag disables *all* runtime safety checks (the above plus bounds checks, enum-cast checks, `!` unwrap, tagged-union field access, `unreachable`)

## Builtins
- Added/Fixed `@fieldParentPtr`, `@typeName`, `@src`, `@panic`, `@trap`, `@fnCtx`, `@min`, `@max`, `@divTrunc`, `@divFloor`, `@rem`, `@mod`, `@{add,sub,mul,shl}WithOverflow`, `@clz`, `@ctz`, `@popCount`, `@abs`, `@mulAdd`, `@targetAbi`, `@targetPtrBits`, `@targetEndian`, `@targetFamily`, `@setEvalRecursionLimit`, `@setMainSymbol`, `@cVaStart` / `@cVaArg`
- Removed libm-dependent math builtins (`@sqrt`, `@sin`, `@cos`, `@tan`, `@exp`, `@exp2`, `@log`, `@log2`, `@log10`, `@floor`, `@ceil`)
- Target builtins return `builtin` enums instead of adding names to the global namespace

## Codegen & tooling
- LTO support
- Reachability analysis / dead-code elimination for `extern` declarations
- Global `var`s with an initializer are no longer implicitly zeroed
- Lexer: word operators (`and` / `or`) only match on a whole-word boundary, so identifiers like `origin` lex correctly
- Formatter: blank lines enforced between aggregates, tests, and functions; fixed a trivia-dropping bug
- Narrow integers now widen implicitly at call args, returns, assignments, and field inits (previously a codegen crash)
- `--emit-gir <file>` / `--emit-llvm-ir <file>` on `build-exe` / `build-obj` / `build-lib` / `test`: write the GIR dump or LLVM IR to a file (both require a path; no dump by default)
- `const x: T = <non-constant expr>` with a mismatched type is now a type error (previously bound directly to the value, skipping the store check)

# v0.2.1

- Update stdx for compressor fix
- Address nit in test subcommand description
- Resolve crash that would occur when invoking the test command on a tree with no test blocks

# v0.2.2

- Same-named `pub` functions, `var` globals, and aggregate methods declared in different modules or types no longer collapse onto one symbol
- A struct / union / enum member or field may now share a name with an unrelated declaration in an enclosing scope (it is only ever reached through `.`)
    - Reusing the enclosing type's own name is still rejected
- A `fn(...): type` constructor whose returned aggregate has `const` function members now compiles
    - Each instantiation gets its own copies of those members (`Vec(i32).make` and `Vec(i64).make` are distinct functions), including `^self` / `&mut self` receivers and calls between sibling members
- Non-generic (`fn(): type`) constructors with member functions, and both generic and non-generic constructors used across a module boundary (`lib::Make()`, `v::Vec(i32)`), are supported
- `mod::Ctor()` for a non-generic `fn(): type` constructor now resolves at compile time
    - Previously only a bare-identifier `Ctor()` did
- Passing two instantiations of the same generic type constructor to another generic (`foo(Vec(i32))` vs `foo(Vec(i64))`) no longer produces a single shared monomorph
- Per-monomorphization body typing and folded `constexpr` arguments now survive cross-module resolution, fixing `[n]T` with a `constexpr n` and type-constructor member `@this()` shapes across module boundaries
- `^r` on a reference-typed value now yields `^T` aliasing the referent (like C++ `&ref`), instead of `^&T` pointing at the reference's own storage
    - `&r` on an already-reference value is still rejected

# v0.3.0

## alpha.1

- Fix a bug where cross module re-exported symbols would break codegen (#199)
- Fix a bug where the LSP would not autocomplete builtin functions (#193)
- Fix a bug where the LSP would not show type information above import statement identifiers (#192)
- Fix a bug where functions would show the GIR '->' return type notation instead of the ':' one in LSP hover (#195)
- Fix a bug where mutability and volatile modifiers would not be included in the stringified representations of types (#194)
- Prevent unsupported declaration modifiers from being used in local functions

## alpha.2

- Add support for freestanding linux (excluding powerpc64le / powerpc)
    - Previously segfaulted due to _start never being defined
- Add `match constexpr` construct for multi-pattern constexpr-pruned 'branching'
    - Pattern are checked to make sure conditions do not overlap
    - Same semantics as `if constexpr`, only resolves the types in the chosen arm
- Add ranges to match expression parser
    - Was handled in emitter but I had missed it in the allowed pattern handles
- Allow multiple patterns to be used in a match arm
    - Captures can be used if all patterns are the same type
- Add profiling hooks to various steps in the compilation pipeline
    - Enabled by building with -Dprofile

## alpha.3

This is a heavily rust inspired release, sorry if that's not your thing!

### General

- Add support for open ranges
    - Use `..` for a new slice
    - Use `..arr.len` for 0 to len (or use `..=arr.len`)
    - Use `lower..` for lower to len
    - These new range syntax only work in subscript operators and as for loop iterables when bounded by a real iterable
- Fixed a bug where whitespace around comments would be accidentally lost
- Fixed a few crashes in the compiler with match arm returns and function pointers
- Writing `[N]&T` / `[]&T` is now an error
    - A reference is a borrow, not a storable slot. Use a raw pointer (`^T`)
- A `[N]&T` / `[]&T` value that still arises (e.g. through a type parameter) decays implicitly to `[N]^T` / `[]^T`, preserving mutability (`&mut` → `^mut`); it never silently gains mutability
- `impl` (inherent and trait, with default methods, associated types, `&mut self` mutation, and `&dyn` dispatch) is verified to work on every aggregate kind: `enum`, `union`, `extern struct`, `packed struct`, `extern union`, `extern packed struct`
- Integer types are now arbitrary width (u123, i3343, etc.) up to 65535
    - Support for `u`, `ul`, `l` suffixes has been removed in favor of `1i2` and friends
    - u0/i0 is not supported
- Adding half, quad, and f80 (x86 only) to floating point types
- Added `constexpr_int` and `constexpr_float` for more explicit coercion rules
- Add atomic builtins
    - These new builtins take in auto params and are type-checked strongly
    - They are not very ergonomic to call outright and are meant to be called through the standard library once an abstraction is in place
- Allow `@tagName` to be used at runtime
- Non-exhaustive enums are supported by `@tagName` properly now
    - A value not in the enum's discriminator set is represented by a `"_"`
- Function types must now have parameter names next to their types

### C ABI hardening

- An `extern` struct / union field may not involve `dyn` in any form (`^dyn I`, `[]^dyn I`, `^^dyn I`, ...) as a fat pointer has no C ABI representation
- The `extern`-aggregate reference check is now transitive: a reference nested behind a pointer / array / slice, or in a function-pointer parameter or return (`^&i32`, `^fn(x: &i32): void`), is rejected, not just a top-level `&T` field

### Interfaces

- `const W := interface { ... }`: a new type-expression prefix beside `struct` / `enum` / `union`.
    - An interface is a first-class `type` value: `constexpr`-composable, `using`-aliasable, re-exportable, usable as a `T: type` argument
- Interface body members:
    - **Required methods**: bodyless `[pub] const name := fn(<self>[, params]): <ret>;`; the `self` form (`self` / `&self` / `^self` / `&mut self` / `^mut self`) is the minimum an impl must provide
    - **Default methods**: same with a body; an impl inherits it unless it provides its own. Default bodies are monomorphized per implementing type
    - **Associated types**: `Name: type;` or `Name: type = Default;`
    - **Associated `const`s**: `const N: T;` or `const N: T = expr;` (`constexpr`-evaluated; may not depend on `@this()`)
    - **`pub` / sealed**: a `pub` member is the external contract; a non-`pub` member is *sealed*: still required of every impl, but only callable from within the interface's declaring module
    - Zero-member marker interfaces are legal (`@implements` tag only)
- An `interface` is **not** a value type: a bare `w: Writer` binding is `error::INTERFACE_NOT_A_VALUE`. It may appear only as `&dyn I` / `^dyn I` or via the `impl I` parameter sugar
    - `&dyn I` / `^dyn I`: two-word fat pointers (`{ data, vtable }`) for run-time heterogeneity; implicit `&mut T` → `&mut dyn I` coercion at call args / assignments / returns / field inits; call-like associated-type binding (`&dyn Iterator(Item = u8)`); one lazily-emitted vtable `const` global per `(I, T)`
- `@this()` inside an interface body denotes the implementing type; inside an impl body it is the target type

### `impl` blocks

- New statement keyword `impl`: a pure statement with no trailing `;`, like `test { ... }`
    - **Trait impl** `impl I for T { ... }`: binds associated types (`using Error = ...`), overrides associated const defaults, supplies the required methods. After it, both `t.method(x)` and `T.method(&t, x)` resolve
    - **Inherent impl** `impl T { ... }`: attaches free-standing methods / statics / aliases to a locally-anchored type declared elsewhere. Multiple inherent blocks coexist as long as no member name collides (`error::DUPLICATE_MEMBER`)
    - **Parameterized impl** `impl(P: type, ..., constexpr n: T, ...) [I for] Ctor(P, n) { ... }`: re-instantiated per monomorphization of `Ctor`, its methods added to every concrete instantiation. Multiple parameters (type and `constexpr`), mixed, are supported; works across module boundaries regardless of which module first materializes the instantiation
- **`impl I` parameter sugar**: `fn f(w: &mut impl Writer)` desugars to a generic function with a synthetic `type` parameter plus a compiler-internal conformance check; a non-conforming argument is `error::UNSATISFIED_BOUND`. Fully monomorphized, zero runtime cost
- **Intersection bounds**: `fn f(x: &mut impl (Reader + Writer))` requires the synthetic param to satisfy every listed interface; the method set is their union.
    - A same-named method from two interfaces makes a bare call `error::AMBIGUOUS_METHOD`; a shared associated-item name is `error::CONFLICTING_ASSOC`
- Static `var` inside a trait impl is allowed
    - Lowers to an `(I, T)`-keyed module global

### Coherence

- **Orphan rule**: `impl I for T` is accepted only in the module that declares `I` or the module that declares the base type constructor of `T`; otherwise `error::ORPHAN_IMPL`. An impl parameter does not count as local
- **Uniqueness**: at most one `impl I for T` program-wide, keyed on canonical `type*` identity (never on name), so `a::Writer` and `b::Writer` are independent and a single type may implement both. A duplicate is `error::DUPLICATE_IMPL`
- Conformance failures are precise: `MISSING_IMPL_METHOD`, `IMPL_SIGNATURE_MISMATCH`, `IMPL_SELF_MISMATCH`, `UNKNOWN_IMPL_MEMBER`

### Builtins

- `@implements(T | value, I)`: a `constexpr bool` is-a predicate usable anywhere a `constexpr` bool is (not tied to `test` blocks). The first argument may be a type or a value; `I` may be an intersection `(A + B)`. Returns `false` for a non-conforming argument, never errors
- `@assert(cond[, msg])`: advisory runtime/comptime assertion calling a new weak `builtin::assert_handler`; comptime-false is a compile error; stripped by `--unsafe`
- `@verify(cond[, msg])`: enforced assertion: comptime-false is a compile error, and the runtime check is **never** elided (not by `--unsafe`, not at any `-O` level)
    - On failure it delegates to the existing weak `builtin::panic_handler`

### Dynamic dispatch: `dyn I`

- `&dyn I` / `^dyn I` / `&mut dyn I` are fully implemented end to end
    - A two-word fat pointer `{ data, vtable }`, a lazily-emitted private `const` vtable per `(I, T)` impl, and vtable-indexed call lowering.
    - Default methods are dispatched through the vtable too
- **Coercion**: a `&T` / `^T` (where `T` implements `I`) implicitly becomes a `&dyn I` / `^dyn I` at call arguments, assignments, returns, and field initializers, building the fat pointer inline. `&dyn A` never coerces to `&dyn B`
- **Associated-type binding**: `&dyn Iterator(Item = u8)`, `&dyn Map(Key = []u8, Value = i32)`. Every associated type of the interface must be pinned here or defaulted (`error::DYN_UNBOUND_ASSOC`); the binding is substituted into the method signatures, so `it.next()` through `&dyn Iterator(Item = u8)` is typed `u8`
- **`dyn`-safety**: a method that takes `self` by value, or names `@this()` outside a `&` / `^`, makes the interface not `dyn`-safe (`error::DYN_BY_VALUE_SELF`); such an interface is still fine for static dispatch
- **`@dynCast(^T | &T, w)`**: an **unsafe**, unchecked `dyn` → concrete recovery: reinterprets `w`'s erased `data` pointer as the target pointer/reference type. No RTTI; the caller owns the risk
- `[]^dyn I` heterogeneous collections iterate correctly; a plain `|x|` for-loop capture of a pointer element (including a fat pointer) is now read by value, not aliased
- `&dyn I` works across a module boundary
- Two impls of the same interface on different local types no longer collide on their emitted method symbol (previously the second impl's method was silently dropped) 
    - This also fixes the equivalent static-dispatch case

## alpha.4
