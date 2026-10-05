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

- Address some compiler errors
    - Shadowing no longer throws an assertion at type resolution
    - Re-exported symbols reference their correct module and no longer cause misaligned memory access
- Add doc comments
    - `///` can lead or lag an ast node and shows on hover through the LSP
    - `//!` at the top of a module shows the comment under the LSP's module hover
- Add `ghoti run` command
    - Eliminates the prior ceremony needed to compile and run a ghoti binary
    - Takes a subset of options from the other build command since this does not need to be as extensible
- Add automatic path detection for sdk on windows to prevent needing to always set an env var or pass through a CLI arg

## alpha.5

- Allow `@compileError` to be used in decl value positions
    - These are fired lazily
- Add zig-like raw identifiers
    - These are declared like `@"asdfasdf"` and can be used to make your identifiers take the name of reserved keywords
    - Formatting will strip unnecessary raw identifiers
- Add string and identifier interning backed by a new compiler-wide arena to save some memory
    - Previously interning was not applied to identifiers and was not applied at the parsing stage
- Fix a compiler error that prevented default functions in interfaces from being used in separate modules
- Fix a compiler error that would falsely error on orphaned impl through re-exports
- Add Writer, Reader, Seeker, and File abstractions to the standard library
- Allow trailing commas in all delimited lists to have the formatter add line breaks
- Add `--emit-asm <file>` on `build-exe` / `build-obj` / `build-lib` / `test`
    - Write the target's native assembly to a file (requires a path; mirrors `--emit-gir` / `--emit-llvm-ir`)
- `[N:0]T` sentinel arrays now report `.len == N`
    - Storage is `N+1`, sentinel lives at index `[N]`
- Fix a bug where struct literal field defaults were never emitted
- Fix an issue where the panic handler would not receive `file` and `msg` args properly
- Resolve bug that prevented an interface from being implemented multiple times
- Resolve formatter bug that would clobber match arms if block had trailing comma
- Properly implement @mem* builtins
    - `@memset`, `@memcpy`, and `@memmove` now all work without libc
- Implement `std::io`
    - Add File abstraction and `Writer`, `Reader`, and `Seeker` interfaces
    - Supported on macos, windows, and x86-64 & aarch64 linux
    - Backs new concrete handlers and test runner in the standard library
- Resolve an issue where aliased types/modules could not be used in dot expressions (bad constant folding)
- Resolve an issue that prevented module-aliased lookups through aggregates
- Add raw memory alloc/free to all backends
- GIR emission is now entirely idempotent
    - This will hopefully reduce a large amount of bugs in the future
    - Checksums are now implemented in debug mode against semantic side tables
- The `::` operator for namespacing has been completely removed 
    - Everything now must route through the `.` operator
- Resolve an issue that prevent discardable method calls from being ignored
- Fix a bug that would discard const correctness checks through address of operations at the IR level
- Add `@embed` builtin for embedding a file on disk at compile time
- Overhaul integer casting semantics with principle of least privilege in casting
    - Added `@intCast(T, x)` and context-inferred `@intCast(x)` for checked integer narrowing and sign conversion with static range verification for compile-time values and hardware-trapping runtime checks
    - Added `@truncate(T, x)` and `@truncate(x)` for explicitly discarding high bits without checking
    - Added `@boolFromInt(x)` and `@intFromBool(T, x)` / `@intFromBool(x)` for explicit bool/int conversions
    - Enabled 1-argument context-inferred forms for `@intCast`, `@truncate`, `@as`, and `@bitCast` across variable/const bindings, assignments, returns, call arguments, and struct field initializers
    - Tightened `@as` to reject narrowing, sign changes, and bool/int conversions with actionable diagnostic suggestions
    - Added rich diagnostics for rejected casts explaining why a conversion was rejected and suggesting the appropriate builtin
- Allow function expressions to discard their parameters at the declaration site rather than needing "_ = param;"
- Overhaul nominal unwrap operators (`?` and `!`) to use the single-match `Flow` protocol
    - Added `builtin.Flow(C, R)` control flow union type with raw identifier variants `@"continue"` and `@"break"`
    - Updated `Option(T)` and `Result(T, E)` in the standard library to implement the single-match `branch()` protocol
    - Refactored GIR lowering to spill `branch()` results to a stack slot and branch directly on the `@"break"` discriminant tag for propagation and error exits
- Add compile-time `impl` and object method evaluation to constant folding
    - `const_eval` now evaluates method calls on objects (`obj.method(...)`) and extension methods registered through `impl` blocks at compile time
    - Bound `self` receiver parameter across value, reference, and pointer receiver forms during compile-time function evaluation
    - Enabled compile-time evaluation of `?` and `!` unwrapping by folding `branch(operand)` calls
- Add semantic safeguards for rvalues and temporary values
    - Prohibit taking mutable references (`&mut expr`) and mutable pointers (`^mut expr`) to temporary rvalues (`error::ILLEGAL_RVALUE_CAPTURE`)
    - Prohibit calling methods requiring `&mut self` or `^mut self` on temporary rvalues
    - Added recursive type-expression detection in semantic analysis to allow mutable pointer/reference syntax in type expressions (e.g. `@typeOf(^mut ^i32)`) without false-positive rvalue errors
- Support trait implementations on primitive types with orphan rule enforcement
    - Traits can now be implemented on primitive types (e.g. `impl Format for i32`) within the trait's declaring module
    - Enforced orphan rules to reject inherent `impl` blocks on primitive types and foreign types (`error::ORPHAN_IMPL`)
- Fix a bug that prevented unions with trailing comments from being formatted
- Implement `errdefer` statement for error-path deferred cleanup
    - Executes deferred cleanup strictly upon early error propagation via the `?` operator
    - Supports capture syntax `errdefer |err| ...` to bind the error payload by value, alias captures (`|^err|` and `|&err|`), and discardable captures (`|_|`)
    - Prohibits mutable capture modifiers (`&mut`, `^mut`, `volatile`) with `ERRDEFER_MUTABLE_CAPTURE`
    - Validates that enclosing function returns a fallible type implementing `builtin.Rewrappable` (`ERRDEFER_IN_INFALLIBLE_FN`)
    - Interleaves with standard `defer` statements in LIFO order on the error propagation edge
    - Prohibits control flow jumps (`return`, `break`, `continue`, `?`) out of `errdefer` bodies
- Resolve a bug that prevented top-level and aggregate-level generic functions from being monomorphized correctly
- Rename `@this` builtin to `@This` to match type constructor and type name conventions
- Resolve an issue that prevented dyn globals from being constructed
- `fn(...) callconv(.x): T`: a function type annotation may specify its own calling convention, matching what a function declaration's signature already accepted
    - Fixed a bug where a mismatched calling convention (e.g. assigning a `.sysv` function to a `.c`-typed variable) went undetected
- A struct/union member value-kind identifier no longer shadows a same-named outer type at a type-position reference, regardless of declaration order
- `fn(T: type, x: T, ...)`: a value parameter typed via an earlier `T: type` parameter is now checked and coerced against T's actual bound value, instead of just taking on its own argument's type independently
    - Lets a single-arg builtin like `@intCast(x)` in that position infer its target type from T
    - A genuinely mismatched argument is now a clean compile error instead of an LLVM signature-mismatch crash
- Several `TypeInfo`-related crashes: mismatched integer widths for `StructInfo.backing_bits`, materializing a slice-shaped compile-time value, and folding `isize`/`usize` type info
- `@as`'s narrowing rejection is now enforced even when its argument folds at compile time; a bare integer literal (`@as(u8, -1)`) still wraps to its bit pattern
- `match constexpr` capture is now foldable in nested constexpr contexts
- Untyped packs (`rest...`) on function parameters, with forwarding (`f(rest...)`) and per-element access (`rest[k]`, `rest.len`)
- `for constexpr` unrolls its block once per compile-time-known element, over any mix of parameter packs, ranges, and `constexpr` array/slice values in parallel (zipped by index), plus an optional trailing companion `0..` index
- `while constexpr` unrolls while its `constexpr var` condition holds, bounded by `@setEvalUnrollLimit`
- `constexpr var`: a compile-time-mutable binding, including aggregate (struct/array) values
- Fix a monomorphization bug in functions that take in a type to determine the types of the other parameters
- Implicit access comparisons now work at compile time
- Arrays/Slices can be sliced with the indexing operator at compile time
- `.ptr` comparison over two compile time arrays now work as intended
    - You cannot store the value of this for runtime use
- Constant folded initializers are not type checked
- Constant array values now correctly read their len field
- Match over an enum with no catch-all now correctly errors with the missing enumerations

### Reflection

- `@typeInfo(T)`: a `TypeInfo` union describing any type's shape (int/float/pointer/reference/slice/array/struct/union/enum/fn/isize/usize/internal)
- `@Int`, `@Float`, `@Pointer`, `@Reference`, `@Slice`, `@Array`, `@Fn`, `@Struct`, `@Union`, `@Enum`: construct a new type from a compile-time descriptor, including synthesizing fresh aggregate types with real fields
- `@hasField`, `@fieldType`: compile-time field introspection
- `@field(value, name)`: reads or writes an instance's own struct/union field by a compile-time-known name (through pointers and references, lvalue-capable)
    - `@field(T, name)`: reads or writes a static `var` or `const` member of a type by a compile-time-known name, the same way `T.member` already does
    - Method and bound access are explicitly rejected

## alpha.6

- Fix a bug that prevented type constructors that did not return aggregates from being used in any meaningful code
- Coercion into `constexpr_int` now folds via constant evaluation
    - The shift fold promotes to 128-bit whenever the shift amount would exceed the narrow domain's width.
- Stop crashing on constexpr negation of integers which should become 128 bit ints
- Fix a bug involving silent wrong answers when multiple instantiations mix
- Fix a crash resulting from a shift expression's LHS is peer-type resolution
- Fix an issue in GIR emission that would crash on usage of some builtin calls without a runtime representation
- Fix a crash resulting from a generic type-constructor's nested member function (like `Option(T).of`) getting permanently marked "resolved" on first visit
- Tests are now only discovered by the root source file's test blocks
    - Placing imports inside of a test block here enables recursive discovery of that source files tests
    - Imports inside of test blocks no longer require an alias as they may just used for discovery
- Resolve an issue where build-obj on any file that imported std would fail to compile and crash
- Imports may now be discarded with an underscore as their alias
- Allow slices to be created from literal implicit access expressions via `^.{a, b, c}`
    - Only possible when the result type is known
    - Constant evaluatable slices are hoisted into static storage
    - A literal can never be mutably taken implicitly
- Calling conventions are now checked per target and do not silently abort codegen with 'Unsupported calling convention'
- `@ptrCast`, `@alignCast` and int <-> ptr conversions now check for ptr types correctly with an error message
- `@Struct` no longer takes a variadic default value string and instead takes an opaque pointer (nullable) for the default value
    - rvalues can now be addressed (`^`/`&`) at compile time for default struct values
- Implicit access/initializer expressions now correctly parse in nested initializer expressions
- The `@Union` builtin no longer takes in any defaults since they were just there to mimic struct behavior and did nothing (was a resolve error, now not even allowed)
- The `NoPayload` marker in the builtin module has been replaced with void in the `typeInfo` union
    - This originally was here to get around a bug involving false name shadowing with raw idents that has since been fixed
- Fix a crash resulting from stack corruption following concatenation of slice types
- Sentinel bytes in arrays and slices of aggregates are now properly zeroed
    - Nullptr is set for ptrs in slices
    - Aggregates are completely zeroed out (instance fields set to 0)
- The `@Enum` builtin now takes in `EnumFieldInfo`'s value as a `constexpr_int` instead of an `i64`

# v0.4.0

## alpha.1

- Fix erroneous "redundant constexpr" diagnostic resulting from constexpr function parameters with a generic `T: type` backing them
- Fix a bug where address-of a constexpr parameter (`^param`) silently failed to fold
- Resolve stale cross-instantiation caching from @Struct(...)/@Union/@Enum call and local constexpr decls
- Fixed a bug where `impl` members never attached to a reflection-built type
- Fixed an issue where volatile would not allow type inference to propagate through the resolver's implicit type stack
- Fixed an issue where volatile globals could never be initialized
    - The constant evaluator can still not read or write to volatile memory post-init
- Resolve an issue that made function pointers always emit monomorphized intantiations
- Add `@returnAddress` builtin to get the return address of the current function as a usize

## alpha.2

## Language Features and Fixes
- **Breaking:** the `@discardable` declaration modifier is replaced by an attribute list; write `@[discardable]` (or `@[discardable(cond)]`) before the declaration, ahead of `pub`
- Attribute lists `@[a, b(args)]` annotate declarations and function literals (`const f := @[discardable] fn(): i32 { ... };`)
    - Arguments are compile-time expressions; unknown, repeated, or misplaced attributes are errors
    - `ghoti fmt` keeps a list beside a declaration head that fits and puts it on its own line otherwise, or when the list ends in a trailing comma (`@[discardable,]`)
    - `@[discardable]` also applies to `extern` function declarations and to function literals
- **Breaking:** the `naked` keyword is removed; write `@[naked]` on the declaration or the function literal (`const stub := @[naked] fn(): void { ... };`)
- `@[inline(.always)]`, `@[inline(.never)]`, and `@[inline(.hint)]` control inlining, backed by the new `builtin.Inline` enum
    - The argument may be computed at compile time: `@[inline(if (FAST) .always else .never)]`
    - `.always` is honored at `-O0` too
    - A function-only attribute written on a declaration applies to its function literal initializer, and is an error on any other declaration
- **Breaking:** `@alignas(n)` is removed; write `@[align(n)]` before the field (`@[align(16)] data: [4]f32`)
- `@[align(n)]` also raises the alignment of globals, static members, locals, and functions; `n` must be a compile-time power of two
- Fixed: an explicitly aligned struct field was accepted but ignored; it now moves the field to its aligned offset and raises the struct's alignment and size, in `@sizeOf`/`@alignOf` and in generated code
    - `align` on a union field is now an error instead of being silently ignored
- Attribute arguments are evaluated per instantiation: a function's attributes see its `type` and `constexpr` parameters, and attributes inside generic bodies and type constructors see the instantiation's bindings
    - `@[inline(if (@sizeOf(T) <= 16) .always else .hint)] const swap := fn(T: type, ...)`
    - `struct { @[align(@alignOf(T) * 4)] value: T }` inside a `fn(T: type): type`
- `@[discardable]` on an interface method applies to calls through `dyn` and `impl` receivers and to the implementing methods
- `@[deprecated]` / `@[deprecated("message")]` on declarations, fields, and interface methods: naming one reports a warning, the compiler's first
    - Uses inside a deprecated item (including a deprecated generic's instantiations) stay quiet
    - Initializing a deprecated field (`.{ .x = 1 }`, `P{ .x = 1 }`, a union's `.{ .legacy = v }`) warns too, including inside generic and type-constructor instantiations, once per site
    - `--deprecated=warn|error|ignore` on `build-*`, `run`, and `test` controls the report; `warn` is the default and never fails the build
    - The LSP publishes it as a warning tagged `Deprecated` (rendered struck through) and hover shows the message
- `@optimizeMode()` returns the build's `builtin.OptimizeMode` (`.debug`, `.release_safe`, `.release_fast`, `.release_small`) and `@runtimeSafety()` whether runtime safety checks are on; both fold at compile time
    - `optimize` and `safety` are also `@cfg` / `@cfgValue` names: `@cfg (optimize == .debug) { ... }`, `@cfg (safety) { ... }`
- `@setRuntimeSafety(bool)` turns runtime safety checks on or off for the rest of its block, nested blocks included; it never reaches into called functions, and `@runtimeSafety()` observes it
- `@branchHint(hint)` as the first statement of an `if`/`else` branch or `match` arm weights that branch (`builtin.BranchHint`: `.none`, `.likely`, `.unlikely`, `.cold`, `.unpredictable`)
    - `@branchHint(.cold)` as the first statement of a function body marks the function cold
- `@typeInfo` of a function declaration reports its attributes in `FnInfo` (`inline_mode`, `naked`, `discardable`, `cold`, `alignment`); a bare function type reports defaults
    - It also reads through member access (`S.f`, `Box(u8).get`); a type constructor's member reports that instantiation's attributes
    - On a generic function it reports the attributes whose arguments ignore the parameters; one that depends on them is an error outside an instantiation, and reflects that instantiation's value inside its body
    - `StructFieldInfo.alignment` reflects a field's `@[align(n)]` (0 when natural), and `@Struct` honors it
    - `builtin.Inline` gains `.default`, which leaves inlining to the optimizer (`@[inline(if (fast) .always else .default)]`)
- Constexpr can now be applied to labels and blocks (expression slots and top level)
    - They must be constant evaluatable and will error if not
- `@assert` and `@verify` have been hardened such that they can work correctly in constexpr contexts
- Resolve an issue where implicit access would not work in some contexts
    - Notably when accessing static constants (non-functions) in generic types
- Do-while and infinite loops can now be marked `constexpr`
    - `do ... while constexpr (cond)` unrolls the loop upto the unroll limit
    - `loop constexpr { ... }` unrolls the loop upto the unroll limit
- Fixes a bug where compile time non-exhaustive enums could emit illegal instructions from `@tagName`
- Fully support `constexpr var` in all compile time contexts
    - This includes indexing, which was currently a blindspot of the evaluator
- Allow control flow in constexpr loops only when in a compile time context
- Add `unreachable` code detection to compile time code execution
- Defer statements (includes errdefer with captures) now work in compile time contexts
- Support global `constexpr var` declarations
- Resolve a codegen error that resulted in misrepresenting signedness in constexpr int literals
- `return`/`break`/`continue` nested inside a match arm's `if` (no `else`) no longer require a trailing semicolon
- The formatter now supports `match` arms whose body is a jump statement
- Fixed a bug where string literals could not be returned from functions
- Builtin handlers now take in a `builtin.SourceLocation` instead of listing the parameters manually
- `builtin.Test` now holds a `builtin.SourceLocation` instead of the members directly
- Rename typeOf builtin to TypeOf
- Make int info signed flag represented by an enum with enumerations `signed` and `unsigned`
- Map `constexpr_int` and `constexpr_float` to their own unique variants in the type info tagged union
- Remove `TypeKind` artifact from builtin types
- Calls to functions returning `[N]T` are now typed as arrays (#338)
    - Previously the result was typed `type` unless the call could be evaluated at compile time. That broke runtime arguments, aliases (`const g := f`), and module-qualified or re-exported calls (`m.f()`)
- Dereferencing a slice whose length is known at compile time copies it into an array: `*s[i..][0..2]` is a `[2]u8` (#339)
    - A length is known for constant-bounded ranges (`x[lo..hi]`), `x[lo..]` over a known-length array or slice, chains like `s[i..][0..n]`, and `const` bindings to any of these
    - `*s` also works as an assignment target
- Assigning to a constant-bounded slice range copies elements in: `buf[i..][0..2] = digits` (#340)
    - The source may be an array, a known-length slice, or `.{ ... }`; lengths and element types are checked at compile time and the destination needs `mut` elements
    - Lowered as a memmove, so overlapping source and destination ranges are safe
    - Assigning to a slice variable (`s = other`) still re-points the slice
    - Works in compile time contexts, which also fixes writes like `buf[i..][0] = v` never reaching `buf` at compile time
- A constant range that runs past a known container length (`a[1..9]` on a `[4]T`) is now a compile error
- Arrays with `const` elements can be copied into `mut`-element storage (`var m: [3]mut u8 = const_arr;`)
    - Copies still cannot gain mutability through pointer or slice elements
- A reference to an array (`&[N]T` / `&mut [N]T`) now implicitly converts to a slice aliasing the array
- Fix nested arrays (`[N][M]T`, `[][N]T`) whose inner dimension was never resolved
- Array sizes from another module are now evaluated in the module that declared them
    - `@sizeOf`, `@alignOf`, `@bitSizeOf`, `@typeInfo`, `@typeName`, and `@implements` on an imported `[N]T` (or a type with one as a field) could silently read the wrong `N`
- `@typeName` reports the declared name of imported aggregates and of aggregates nested inside compound types (`^Point`, `[]Point`, ...)
- Fix generics whose body-local declarations were not re-typed per instantiation
    - e.g. `var a: @TypeOf(value)`, `var a: T`, or a `using` alias built from `@typeInfo(@TypeOf(value))` kept the first instantiation's type
- Errors inside an imported generic's instantiation are now reported against the imported module, instead of a bogus location in the importer
- `@TypeOf(x)` inside a compound type annotation (`^@TypeOf(x)`, `[]@TypeOf(x)`, `[N]@TypeOf(x)`) now denotes the type instead of `type`
- `@Int` / `@Float` / `@Pointer` / `@Reference` / `@Slice` / `@Array` / `@Fn` are recomputed for each generic instantiation
- Type mismatch diagnostics print full types (`'^u8'`, `'&i32'`, `'[2]u8'`) instead of just the kind (`'pointer'`, `'array'`) (towards #276)
- Passing a reference where a pointer is expected now errors with "A reference does not implicitly convert to a pointer", plus a hint to use `^` instead of `&` when the value is a `&` expression
- Fix a crash emitting object files that contain compile-time-only functions (those returning `type`)
- `ghoti test --emit-llvm-ir` no longer tries to lower unreachable compile-time-only functions
- Remove the leftover `::` operator token
- Resolve a bug where break values would not be cleared on re-resolutions of monomorphs
- Unions can now have fully void payloads without crashing
- Function parameters can now safely load void parameters
- **Breaking:** the `using` keyword is removed (#320); declare type aliases with `const` instead (`using A = i32;` becomes `const A := i32;`)
    - Whether a declaration aliases a type or binds a value is decided by what its right-hand side denotes, so `const P := ^i32;` is a type and `const p := ^x;` is a pointer
    - `constexpr` may be used in place of `const` for an alias with no change in meaning; aliases never occupy storage
    - `using` is now an ordinary identifier
- Local `const` / `constexpr` aliases of a type constructor call (`constexpr NewRes := Result(T, E);`) are compile-time types instead of runtime calls, fixing an LLVM crash (#334)
    - Module-scope aliases of a module (`const io := std.io;`) and of `void` no longer emit storage
- `dyn I`, `opaque`, `type`, and `noreturn` are valid in value position (`const Any := &dyn Writer;`, `const Handle := ^mut opaque;`)
- `^` / `&` over a `type` value (`^@TypeOf(x)`, `^fn(a: i32): i32`) points at the denoted type
- A type used where a value is expected is now an error (`const p: ^i32 = P;` where `P` is a type)
    - Covers annotated declarations, assignments, returns, call arguments, and aggregate / array elements
- A `var` binding whose value is a type (`var T := i32;`) is rejected like `var T: type = i32;`
- `^` / `&` applied directly to a struct, union, enum, or interface literal is rejected in value position as it already was in type position
- Fix `@sizeOf(Ctor(T))` in a generic body folding to another instantiation's layout
- Fix `@sizeOf` / `@alignOf` / packed-field sizing of `&dyn I` / `^dyn I` fat pointers (two words, not one)
- `^f` of a function converts to a `^fn(...)`, and `*p` of a `^fn(...)` is the callable itself; calling through a local, field, or module-scope `^fn(...)` / `var fn(...)` no longer crashes
- Struct and union fields typed by a `type` value (`f: @TypeOf(g)`, `f: FnAlias`) store the denoted type, so function-typed fields are callable
- Non-generic type constructors that return an existing scalar type (`fn(wide: bool): type { return i64; }`) now fold, so values annotated with them are typed correctly
- **Breaking:** a bare `fn(...)` type is an erased callable: a two-word `{ctx, code}` value that holds either a plain function or a capturing closure
    - Usable as a local, parameter, return type, aggregate field, or array element; `var f: fn(n: i32): i32 = add;` and `f = some_closure;` both work
    - A capturing closure passed to a `fn(...)` parameter is erased instead of monomorphizing the callee per closure; use an `auto` parameter for the zero-cost, specialized form
    - Returning a capturing closure through a `fn(...)` return type is an error (even with `move fn`), since the closure's captures live in the returning frame; return it by its own type with an `auto` return instead
    - `&fn(...)` is accepted and means the same as `fn(...)`; `^fn(...)` is the same two words but nullable and compares against `nullptr`; `&mut fn` / `^mut fn` are rejected
    - An erased `fn(...)` cannot be an `extern struct` / `extern union` field, and a C-variadic function never converts to one
- `extern fn(...)` is the thin, C-ABI function pointer that a bare `fn(...)` used to be
    - `callconv(...)` on a function type now requires `extern fn`
    - `extern` / `export` declarations (`extern("kernel32") const f: fn(...): R;`) and `constexpr` function parameters keep the thin type automatically
    - `builtin.Test.func` is now `extern fn(): bool`
- `dyn Fn(name: T): R` is accepted as another spelling of `fn(name: T): R`, including behind `&` / `^`
    - A user-defined `interface Fn` still works with `dyn Fn` / `dyn Fn(Out = T)`
- A function literal's return type may be a function type written directly before its body: `fn(): fn(n: i32): i32 { ... }`
- A call's result can be called directly: `make()(1)`, `make_maker()()(40)`
- `builtin.FnInfo` gains `erased: bool = true`; `@typeInfo(T).function.erased` tells an erased `fn(...)` from a thin function type, and `@Fn` builds the same type the spelled-out `fn(...)` / `extern fn(...)` would
    - `@Fn` rejects an erased descriptor that has `has_self` or a calling convention other than `.c`
- `if constexpr a else b` without a condition runs `a` when evaluated at compile time and `b` at runtime (#336)
    - Compile-time evaluation means `constexpr { }` blocks and labels, `constexpr` declaration initializers, and other compile-time folds; both branches are type checked
    - `if constexpr (cond) ...` with a parenthesized condition is unchanged
- Functions with a parameter pack (`rest...`) can be called at compile time: `rest.len`, `rest[k]`, `for constexpr (rest)`, and `f(rest...)` forwarding all fold (#336)
- In a `constexpr`-declared function, a parameter read by a compile-time construct (`if` / `match` / `for` / `while constexpr` headers, `constexpr` blocks, local `constexpr` initializers) is implicitly `constexpr` (#337)
    - Operands of `@TypeOf` / `@sizeOf` / `@alignOf` / `@bitSizeOf` and `T: type` parameters don't count
    - Passing a runtime value to such a parameter reports the usual call-site error with a note explaining why the parameter is `constexpr`
- A bare `dyn I` can be aliased without indirection (`const Bound := dyn io.Writer(Error = io.Error);`) and used as `&Bound` / `^Bound` (#328)
- An interface or bare `dyn I` used by value as a field, parameter, return type, local, or array element is now rejected with a hint to use `&dyn I` / `^dyn I`
    - Previously only locals were checked
- Fix a defaulted associated type on an interface from another module resolving to garbage through `&dyn I`, which also crashed the LSP (#317)
- `&dyn I(Out = i32)` and `&dyn I(Out = u8)` are now distinct types instead of silently interchangeable
- Diagnostics print interface names and full `dyn` types (`dyn Sink(Out = i32)`) instead of `interface` / `dyn`
- A function-type alias used as a return type (`fn(): Callback`) now returns a callable value
- A generic instantiated with both a thin and an erased function type argument now produces distinct instantiations
- Function types rebuilt while substituting unwrap shapes no longer drop their calling convention
- Diagnostics show full types everywhere instead of bare kinds like `array` / `slice` / `enum` (#341)
    - `Type '[3]i32' has no field named 'x'` (previously the variable's name or `array`), `Expression of type 'S' is not callable`
    - Unary `-` / `!` / `~` and non-`bool` conditions report the operand type they found
    - Binary operator errors use the source spelling (`'+'`, `'<<'`) instead of internal names (`'add'`, `'shl'`)
- Saturating operators `+| -| *| <<|` and compound forms `+|= -|= *|= <<|=` clamp to the operand type's range instead of wrapping or trapping (#321)
    - `<<|` saturates for any shift amount, including one at or past the bit width
    - Fold at compile time; between two untyped integer constants they fold like the plain operator
- `@backingInt(x)` returns the integer backing an enum, a `packed struct` / `packed union`, or a tagged union's active tag; `@fromBackingInt(T, n)` (or `@fromBackingInt(n)` with an inferred `T`) converts back for enums and packed aggregates (#327)
    - Converting an unlisted value to an exhaustive enum panics under runtime safety
- **Breaking:** `@as` no longer converts between enums and integers; use `@backingInt` / `@fromBackingInt` (#327)
- `@intFromFloat(T, x)` truncates a float toward zero and `@floatFromInt(T, x)` rounds an integer to the nearest float; both infer `T` from context when omitted (#324)
    - An out-of-range compile-time `@intFromFloat` is a compile error; at runtime an out-of-range or NaN operand panics under runtime safety
- **Breaking:** `@as` no longer converts floats to integers, or integers to floats that can't represent every value exactly (`@as(f64, i32_val)` still works); use `@intFromFloat` / `@floatFromInt` (#324)
- `import` accepts an absolute path (`import "/abs/path/lib.gh" as lib;`), resolving to the same module as any relative spelling of that file
- `undefined` can be aliased with a `const` binding (`const U := undefined;`); every use of the alias behaves like the literal
    - `@TypeOf(undefined)` names its type, which only a `const` binding may have; `var` bindings, parameters, return types, fields, and array elements of that type are rejected
- **Breaking:** `@typeName(T).len` no longer counts the trailing null terminator, matching string literals
- Hexadecimal float literals: `0x1.8p3`, `0x1p-4`, `0xA.8`; the `p` exponent is a decimal power of two, and a width suffix (`0x1.8p3f32`) may follow it
- A compile-time float that would round to infinity in its type (`const x: f32 = 1e300;`, `const h: f16 = 70000;`, `@as(f32, BIG)`, `@floatFromInt(f16, 100000)`) is now a `LITERAL_OUT_OF_RANGE` error instead of silently becoming `inf`
- Fixed: an untyped integer literal above `i128` max (up to `u128` max) was treated as negative, so `340282366920938463463374607431768211455 < 0` was true and converting it to a float gave a negative value
- Compile-time floats are exact IEEE binary128 values computed in software, never a host `double`
    - A typed operation rounds once in its type's format, so a folded `f16`/`f32`/`f64` result matches runtime bit for bit
    - `f80` and `f128` constants now fold at compile time with full precision (`const q: f128 = 1.0 / 3.0;`)
    - A typed literal is read straight from its digits into its type, never rounded twice
    - Untyped float constants carry `f128` range (about 1.19e4932) until they meet a type
    - A nonzero literal that would round to zero in its type (`const x: f32 = 1e-50;`) is a `LITERAL_OUT_OF_RANGE` error
    - `@intFromFloat` folds across the whole 128-bit range
- Signed integer overflow in a compile-time expression with a concrete type (`i32` max `+ 1`, `-i32_min`, `min / -1`, including `i128`) is an error, matching the runtime overflow panic; unsigned results and shifts wrap to the type's width like runtime
- Fixed: 64-bit compile-time integer arithmetic could overflow inside the compiler (`111334094107016374 * 242`) or wrap modulo 2^64 (`9223372036854775808 * 4` folded to 0); it now folds exactly
- Fixed: a runtime `@mulAdd` crashed the compiler; its operands are now coerced to and checked against `T`
- Fixed: comparing floats of different widths (`f32 < f64`), or an untyped integer result with a float (`@abs(3) < x`), crashed code generation
- Fixed: a typed module constant built from an untyped constant (`const a: f32 = big;`, `const b: u8 = two_hundred;`) folded as the untyped type, crashing `@bitCast` and friends; an integer constant bound to a float global (`const f: f32 = five;`) crashed too
- Fixed: nested untyped constant arithmetic next to a typed float (`((c * c) + c) * f32_value`) was rejected
- Fixed: a compile-time-false `@assert` in an `if` or `match` arm that a folded condition rules out was an error, e.g. `if (N > 4) { @assert(N > 4); }` with `N = 2`, including inside generic instantiations
- `@[visibility(.default)]`, `@[visibility(.hidden)]`, and `@[visibility(.protected)]` set a symbol's visibility, backed by the new `builtin.Visibility` enum
    - Allowed on `pub`, `export`, `extern`, and `weak` declarations; `.protected` only on ELF targets
    - `@typeInfo(f).function.visibility` reflects it
- Fixed: several malformed programs crashed the compiler instead of reporting an error, including `@hasField` with a non-type or non-string argument, `return 1.5` from an integer function, `-i64`, `@bitCast(undefined)`, a `noreturn` field or variable, a pack parameter used as a return type, and a match mixing type and value arms
- Fixed: code after a `match` or `if` whose arms all return is treated as dead instead of miscompiling
- Compile-time evaluation and runtime code now share one definition of every integer and float operation, and a differential test checks that each folded result is bit-identical to the one computed at runtime
    - **Breaking:** float division by zero folds to an infinity or NaN like it runs, instead of being a compile error
    - **Breaking:** `@divTrunc`, `@divFloor`, `@rem`, and `@mod` panic on division by zero and on `MIN / -1` under runtime safety, and signed `@abs(MIN)` panics; each was already a compile error when folded
    - `x <<% n` with `n` at or past the bit width is `0` both folded and at runtime
    - Float `@min` / `@max` are IEEE `minimumNumber` / `maximumNumber`: a NaN operand loses and `-0 < +0`, folded and at runtime
    - Float `%` folds exactly, with the sign of the dividend, like it runs
    - `f80` operands with an encoding x87 rejects (unnormals, pseudo-infinities, pseudo-NaNs) fold to NaN like they run
- Fixed: a float `!=` with a NaN operand was `false` at runtime (it folded to `true`); `x != x` now detects NaN everywhere
- Fixed: `@floatFromInt` of an integer wider than 113 bits could round twice when folded
- Fixed: a float `@as` never folded, so `const x: u8 = @intFromFloat(u8, @as(f32, 3.5));` was rejected at module scope
- **Breaking:** `@floatFromInt` of a typed integer past the target's range folds to an infinity, like it runs, instead of being a compile error; an out-of-range untyped literal is still rejected
- Fixed: on Windows, `f80` arithmetic and conversions rounded to `f64` precision, because the x87 unit starts in 53-bit mode there; executables and test binaries now switch it to full 64-bit precision at startup
- Fixed: a `@as`, `@bitCast`, `@intCast`, or `@truncate` operand took its type from the surrounding expression, so `@bitCast(f32, @as(u32, 5)) >= x` treated `5` as a float and was rejected
- Fixed: a type declaration emitted a zeroed global the size of the type, so declaring a huge type (`struct { data: [100000000000]u8 }`) ran the compiler out of memory writing the object file
- Fixed: more malformed programs crashed the compiler instead of reporting an error, including a type whose layout depends on its own `@sizeOf`/`@alignOf`, assigning to an array's `.len`, indexing a block, `@cfg` inside an `if` used as an operand, and passing a type (or the `type` keyword) where a value is expected, including to a generic `[]T` parameter
- Fixed: some operations on 128-bit and odd-width integers (`+|`, `-|`, `-%`, `~`) could not be folded, or folded to a different value than the one computed at runtime
- Values of different types now meet at their *peer type*: the one operand type every other widens into
    - Mixed-width arithmetic, bitwise operators, and comparisons convert both sides first: `an_i8 + an_i64` is an `i64`, `a_u32 < an_i64` compares as `i64`, and `an_i32 * an_f64` is an `f64`
    - The arms of an `if` or `match` and the values a label is broken with meet the same way, replacing "the first arm decides"; an arm that leaves (`return`, `break`, a `noreturn` call) takes no part
    - `@min`, `@max`, `@divTrunc`, `@divFloor`, `@rem`, `@mod`, and the `*WithOverflow` builtins convert their operands to the peer type (`@shlWithOverflow`'s count keeps its own)
    - Pointers and slices meet at the least mutable (`^mut T` with `^T` is `^T`), `nullptr` takes the pointer's type, and arrays of different lengths meet as a slice of their element
    - No type is invented: `i32` with `u32`, or `i64` with `f64`, is a `NO_PEER_TYPE` error naming both types and the cast to write
    - Compile-time folding converts operands the same way, so a folded mixed-type expression equals the one computed at runtime
- Fixed `f(fn(x: i32): i32 { ... })[i]`: a function literal passed to a call whose result is indexed crashed the compiler
- A module-level `const` with an array annotation now checks its initializer against it (`const P: [3]u8 = "ABC";` is a type mismatch, as it already was inside a function)
- A call folded at module scope now rejects a number passed for an array, slice, struct, union, or function parameter
- A type is rejected as a range bound, as an asm input (including the `type` keyword), as `@backingInt`'s operand, and as an atomic builtin's operand; `@cVaArg` requires a concrete value type; a binary operator rejects an `undefined` operand
- `@export(f, .{ .name = "sym", .linkage = .weak, .visibility = .hidden })` defines a function under another symbol name, once per name, like Zig's `@export`; `builtin.ExportOptions` and `builtin.Linkage` describe the options
    - **Breaking:** `@setMainSymbol` is removed; write `@export(entry, .{ .name = "main" })` in the root module instead (the entry needn't be `pub`)
    - An exported function (`@export` or `export(...)`) in an imported module is no longer dropped when nothing calls it
    - `@export` also takes a function passed to a `constexpr` parameter, so a Zig-style `symbol(f, name)` helper works
- Fixed a type constructor's member calling a function passed to its `constexpr` parameter (`Wrap(i32, f)` with `f(self.val)` in a method) crashing the compiler
- Fixed a function passed to another module's `constexpr` parameter resolving by name in the callee's module (it could call the callee's own function of the same name)
- A keyword is a name right after `.`: `.weak`, `x.type`, `.{ .export = 1 }`; declaring such a field or variant still takes `@"..."`
- Unicode escapes: `\u{H...}` encodes a scalar value as UTF-8, and `\xHH` is one raw byte, in strings, character literals, and raw identifiers (#325); a malformed escape is reported at the escape
- **Breaking:** a character literal is its code point, an untyped integer constant that defaults to `u21` (`var c := 'a';` is a `u21`); it still coerces to `u8` wherever one is expected, and `'é'`, `'😀'`, and `'\u{1F600}'` work. A literal with more than one code point is an error
- Raw identifiers must be valid UTF-8, match byte for byte after decoding escapes (no normalization), and stay raw when formatted (#326)
- Diagnostics place the caret by display width, so it lines up after emoji, CJK text, and combining marks; a diagnostic's file path prints as UTF-8 on Windows instead of through the code page
- `ghoti lsp` speaks UTF-16 positions by default and UTF-8 when the client offers it (`positionEncoding`), so hover, go-to-definition, references, rename, diagnostics, and edits land correctly on lines with non-ASCII text
- `ghoti fmt` measures line width in display columns
- `@sqrt`, `@sin`, `@cos`, `@tan`, `@exp`, `@exp2`, `@log`, `@log2`, `@log10`, `@floor`, and `@ceil` are back (#352)
    - Folded at compile time with correct rounding in every float type, including exact reduction of huge trigonometric arguments; an untyped result is computed again in the type it lands in rather than rounded twice
    - At runtime `@sqrt`/`@floor`/`@ceil` use LLVM's intrinsics and the rest call the target's math routines directly (never `llvm.sin` and friends, which LLVM folds with the host's libm)
- `@floatCast(T, x)` converts between float types, narrowing or widening, rounding to nearest (#349)
    - A compile-time finite value that overflows `T` is a compile error; at runtime an overflow rounds to an infinity, with no safety check
    - **Breaking:** `@as` no longer narrows a float (`@as(f32, some_f64)`); use `@floatCast`. Widening and literal coercion (`@as(f32, 0.1)`) are unchanged
- `@TypeOf(a, b, ...)` takes any number of operands and returns their peer type, in type positions, `if constexpr` conditions, and generic signatures (`fn(a: auto, b: auto): @TypeOf(a, b)`)
- Fixed: a local declared in a loop body took new stack space on every iteration, so a long loop with a large local overflowed the stack
- Fixed: an `if` or `match` used as a statement was rejected when its arms had different types (`if (c) x = 1 else flag = true;`)
- Fixed: `if (c) value else return e` stored a `void` into the result instead of skipping the arm
- **Breaking:** declarations are `const` (known at compile time), `let` (an immutable runtime value), or `let mut` (a mutable one), replacing `constexpr`, `const`, and `var`
    - A compile-time mutable local is `comptime let mut` (was `constexpr var`); `comptime let` is an error, since that is `const`
    - Types, `undefined` aliases, and values holding `type`s are declared with `const`; a closure that captures runtime state is declared with `let`
    - `extern`, `export`, `weak`, and `threadlocal` data needs `let` or `let mut`; a function with linkage is declared with `const` (`pub weak const panic_handler = fn ...`, `extern const puts: fn(...)`)
    - A `let` still folds where a compile-time value is needed (`let n = 4; let a: [n]u8 = ...`); `let mut` never does
- **Breaking:** the walrus operator `:=` is removed; an unannotated declaration is written with `=` (`const x = 5;`, `let mut n = 0;`), as are interface methods (`const area = fn(self): f64;`)
- **Breaking:** every other `constexpr` is spelled `comptime`: parameters (`fn(comptime n: usize)`), `impl(comptime N: usize)`, `if`/`match`/`for`/`while`/`loop comptime`, `do ... while comptime`, and `comptime { ... }` / `comptime name: { ... }` blocks
- **Breaking:** `constexpr_int` and `constexpr_float` are renamed `comptime_int` and `comptime_float` (and the matching `builtin.TypeInfo` variants); diagnostic codes named `CONSTEXPR_*` are renamed `COMPTIME_*`
- Every function declared with `const` infers its compile-time parameters: a parameter its body reads at compile time (`if comptime (a < b)`, `const s = @sizeOf(@TypeOf(x))`) is compile-time at each call, as only `constexpr`-declared functions did before
- `comptime <expr>` evaluates one expression at compile time (`let table = comptime build(64);`, `comptime validate(fmt);`); it binds like a prefix operator, so `comptime f(x) + y` folds only `f(x)`, and writing it where evaluation is already compile-time is an error
- Fixed: a `void` function whose body runs off its end couldn't be called at compile time (`comptime { check(); }`)
- A `comptime let mut` can hold a type (`comptime let mut T: type = u8; T = Wrap(T);`); each use names the type it holds at that point, including across `for comptime` and `while comptime` iterations, and only a type can be assigned to it
- Fixed: a function that fills an `undefined` array or struct one element or field at a time (for example in a runtime `while` loop) couldn't be evaluated at compile time; reading an element that was never written is now a compile error
- Fixed: compiler crashes on four kinds of invalid code, which are now errors:
    - a closure assigning to a captured binding that isn't a `let mut` (a `let`, a parameter, or a loop capture); writing through a captured slice, pointer, or reference still works
    - a range nested inside a `for` iterable or subscript instead of being the whole of it (`for (n = 0..3)`, `a[blk: { break :blk 0..1; }]`)
    - a member reached through a slice, array, pointer, or function type rather than a value of it (`[3]i32.len`, `(fn(): S).x`)
    - a generic function declared C-variadic (`fn(a: auto, ...)`); use a parameter pack instead
- Fixed: match patterns missing a comma (`.a .b => x`, which parsed as a member of `.a`)
- `mut?` lets one function serve both mutable and immutable callers: `&mut? T`, `^mut? T`, `[]mut? T`, and `[N]mut? T` take their mutability from the call
    - `pub const at = fn(&mut? self, i: usize): &mut? T { return &mut? self.items[i]; };` returns `&mut T` through a `let mut` receiver and `&T` through a `let` one; a call is mutable only when every argument bound to a `mut?` parameter is
    - Inside the function a `mut?` view can be read, narrowed (`&mut? self.items[i]`, `|&mut? v|` in `match` and `for`), or passed to another `mut?` function, but not written through
    - `&mut? x` needs a place reached through a `mut?` parameter, and a struct or union field, a global, or a function without a `mut?` parameter can't use `mut?`
    - An interface's `mut?` method must be implemented with the same `mut?` signature, and can't be called through `dyn`
    - Generic functions work the same way: `fn(T: type, s: []mut? T): &mut? T` returns `&mut T` for a mutable slice
- **Breaking:** `builtin.Unwrappable.branch` is now `fn(&mut? self): Flow(&mut? Output, Residual)`, so it hands back a reference into the operand instead of a copy of its payload; an impl writes its success arm as `.some => |&mut? v| .{ .continue = v }`. `?` and `!` read through that reference and are otherwise unchanged
- `if` and `while` unwrap any `builtin.Unwrappable` (`Option`, `Result`, ...) with a capture after the condition: `if (opt) |v| { ... } else { ... }`, `if (res) |v| v else |e| fallback(e)`, `while (it.next()) |item| : (i += 1) { ... } else |err| { ... }`
    - The capture takes the same forms as a `match` arm: `|v|`, `|&v|`, `|&mut v|`, `|^v|`, `|^mut v|`, `|&mut? v|`, and `|_|`; `|&mut v|` writes into the operand, which must be a mutable place
    - `else |e|` captures the residual by value; it is an error when the residual is `void` (an `Option`), and needs a capture after the condition
    - An `Unwrappable` condition without a capture is an error (write `|_|` to ignore the payload), and a capture on any other condition (a `bool`, a pointer) is an error
    - `while` calls `branch` before every iteration and runs its `else` when it breaks; a `break` statement skips the `else`, as before
    - The payload capture is also in scope in the continuation: `while (it.next()) |x| : (sum += x)`
    - Everything works at compile time, including `if comptime (x) |v|`, `while comptime (x) |v|` (by-value captures), and constants such as `const X = if (OPT) |v| v else 0;`
- Fixed: at compile time, a `?` that returned early from inside a `let` initializer, an expression statement, or a `return` made the whole call fail to evaluate
- Fixed: `&mutex`, `^mutable`, and other names starting with `mut` right after `&` or `^` were split into `&mut` and the rest of the name
- Fixed: compile-time evaluation copied whatever a reference or slice pointed at, so writes through it were lost and some calls folded to a different value than at runtime. References, pointers, and slices now refer to the variable or element itself:
    - `let p = &mut x; p = 8;`, a `match` arm's `|&mut v|`, and a `for` loop's `|&mut e|` gave the old value; they now write through
    - writing through a slice of an array (`fill(arr)`, `let s = arr[1..]; s[0] = 5;`) changed only a copy; it now changes the array
    - `&mut` parameters, `^mut` pointers, and `&mut self` methods called through a reference can now be evaluated at compile time
    - a pointer into an array steps through its elements: `let p: ^mut i32 = a.ptr; p[2] = 4;`
    - returning a reference to a callee's own local, or using a reference after its variable's scope ended, is a compile error
    - passing `&mut x` of a `let mut` local to a function that matches on it could fail with "Non-exhaustive match in compile-time constant evaluation", because the call was folded with a copy of `x`'s initializer
- Fixed: a parameterized `impl` could expand for a type constructor from another module (such as the prelude's `builtin.Flow`) whose declaration happened to sit at the same position as its own, so unrelated edits made errors like "Cannot take a reference to an already-reference-typed value" appear inside the `impl`
- Fixed nine compiler crashes:
    - an array or struct field of type `void` (`[3]void`, `struct { a: void, ... }`); it now takes no space
    - a module constant referring to a literal (`const N = &22;`, `const P = ^@as(i32, 7);`) when read at runtime
    - an array literal whose element type is `noreturn` (`[3]noreturn{...}`), which is now an error like any other `noreturn` slot
    - a member of a method reference (`let make = S.make; make.item`), which resolved against the method's return type; it is now an error
    - calling a capturing function literal where it is written (`fn(x: i32): i32 { return x + offset; }(5)`)
    - comparing two `void` values (`s.a == {}`), which now folds to `true`
    - naming a `dyn` method without calling it (`v.x + 1`), which is now an error
    - comparing an untyped integer expression with a float literal in a condition (`if (0 - 2 != 0.25)`), which now folds
    - a `&` or `^` self parameter with no name (`fn(&): i32`), which was silently dropped and is now a syntax error
- **Breaking:** `@ptrFromArray(a)` is removed; `a.ptr` already gives the same pointer to the first element, for arrays and slices alike
- **Breaking:** a `for` range over untyped bounds (`for (0..3) |i|`) counts in `usize`, or in `isize` when a bound is negative, instead of `i32`
    - when the bounds are known at compile time, the counter also converts implicitly to any integer type that holds every value it takes, so `s += i` with an `i32` `s` and `take_u8(i)` keep working
    - `let x = i;` still gets the counter's own type, so `return x` from an `i32` function needs `@intCast`
    - `for comptime` is unchanged: it unrolls into constants of the default integer type
- Assignment into a `comptime let mut` aggregate can go any number of levels deep through fields and elements (`p.a.b = v`, `arr[i].xs[j] += v`); only one level was supported before
- A `for` range whose constant bounds are the wrong way round (`for (5..3)`) is an error, since ranges only count up; with runtime bounds it runs zero times
- Fixed: `lo..=hi` never stopped when `hi` was its type's maximum (`for (a..=b)` with `b: u8 = 255`), since the counter wrapped around
- Fixed: a `for` over `lo..=hi` in a function run at compile time left out `hi`
- Fixed: taking the address of a `for` range capture (`&i`) crashed the compiler
- `\\` multiline strings format like Zig's: after `=` they start their own indented line, and a list never gets a trailing comma alone on the line after one
- Returning `&x` or `^x` of a local, a by-value parameter, or a field or element of one is an error (`ESCAPING_LOCAL_REFERENCE`), since the reference would outlive the function's frame
- A loop without a label used as a value (`let r = while (c) { ... } else 7;`) is an error instead of being typed as an internal block; a labeled loop yields its `else` value even when nothing breaks out of it
- A generic function can call itself with the same arguments (`fact(T, n - 1)`); one with an inferred `auto` return type that does is reported instead of exceeding the instantiation limit
- Fixed: `.{ ... }` couldn't initialize a `[n]T` parameter sized by a `comptime` argument (`sum(3, .{ 1, 2, 3 })`)
- Fixed: `let p: ^T = nullptr;` read as the untyped `nullptr` rather than a `^T`, so `if (p)` and `!p` were rejected
- Fixed: an implicit `.field` naming a struct field (not a constant or function) resolved to the field's type; assigning it to a packed struct compiled silently. It is now an error
- Fixed seven more compiler crashes, now errors or working code:
    - a `fn(...): type` constructor taking an untyped value pack (`fn(c...): type`)
    - `@bitCast` of an untyped float to a type that isn't 8 bytes (it has `f64`'s bit pattern)
    - a closure assigning a binding declared after it
    - a type used as an `if` or `while` condition (`if (bool)`)
    - `_` as an `asm` input operand
    - calling a static member that holds a function pointer (`const open = ^g;`, `S.open(7)`)
    - a type written where a value argument goes (`f(impl a)`)
- A parameter used as an array length (`let buf: [n]T`) is inferred `comptime` like any other compile-time read
- Fixed: a `for` loop couldn't capture `|&mut e|` over a `let mut` array whose type was written out (`let mut buf: [3]mut u8 = ...`)
- Fixed: a module constant whose initializer reads itself is reported as a cycle; reading it more than once used to take exponential time before failing
- Fixed more crashes on invalid code, now errors:
    - a type where a value belongs: `@ptrCast(^mut opaque, ^mut u8)`, `if (b) u8 else 1`, a type pattern in a `match` on a value
    - naming an `impl` method of a primitive without calling it (`val.format`)
    - a global array initialized with a non-array (`let mut buffer: [4]u8 = true;`)
    - a non-function passed to a `comptime f: fn(...)` parameter
    - a `const` static member whose initializer reads a `let mut` one
- Fixed crashes on valid code: `-2 != 0.25` in a condition, and `@shlWithOverflow(a, 3, &mut out)` with an untyped shift count
- Fixed: a function literal passed straight to a `comptime f: fn(...)` parameter (`apply(fn(n: i32): i32 { return n + 1; }, 4)`) was rejected as not compile-time
- Fixed: an argument for a `y: @TypeOf(x)` parameter was never checked against or converted to that type, so `echo(1, true)` compiled and `echo(a, @as(i16, 2))` crashed
- `auto` under pointer, reference, slice, and array levels restricts what it deduces from (#362): `x: &auto`, `p: ^mut auto`, `xs: []auto`, `xs: []mut auto`, `xs: [3]auto`, `x: &mut? auto`, and nestings like `&[]auto` or `^^auto`
    - each level must match the argument and `auto` binds what's left, so `&auto` given `&mut i64` makes the parameter `&i64`
    - `impl I` works in the same positions (`xs: []impl Area`, `x: &impl Area`) and checks `I` against what `auto` bound to
    - **Breaking:** an argument that doesn't match is an error at the call; before, `x: &auto` and `x: ^auto` silently took a plain value and became that value's type
- `impl Fn(v: T): R` parameters accept any function, closure, erased `fn` value, or `^fn` whose signature is exactly that, and report a mismatch at the call (#362)
    - `auto` in the signature deduces from the callable: `impl Fn(v: T): auto`, `impl Fn(x: auto): bool`
- Fixed: `impl I` bounds were never checked for a call from another module or from inside another generic's body
- Fixed: a plain `auto` parameter given a reference (`f(&a)`) failed with a mismatch between the reference and its referent
- Fixed: a closure in a `test` block that captured one of the test's locals crashed the compiler
- Fixed: the code after an `if comptime` arm that always returns was still checked, so an `@assert` or a union field read past it failed on a path that never runs
- Fixed: a `match` statement with a block arm next to a value arm (`1 => { ... }, _ => @expect(false)`) tried to store the block's `void` as the other arm's value
- Fixed: a type constructor called through an alias (`const A = Box;`) or another module (`m.Box(u8)`) couldn't be compared at compile time (`A(u8) == A(u8)`)
- Fixed: a crash resolving a generic call whose arguments themselves instantiated another generic
- Fixed: distinct instances of one type constructor were treated as the same type: `let b: Box(u16) = a` with `a: Box(u8)` compiled, passing one to the other crashed the compiler, and `Box(u8) != Box(u16)` was false
- Diagnostics and `@typeName` name a type constructor's instance by its arguments (`Box(u8)`, `Arr(i32, 4)`) instead of `struct`
- Fixed: a type constructor member whose signature names its own constructor (`fn(&self): Pair(i64)` inside `Pair`, or a dependent `Result(U, E)` return inside `Result`) recursed forever, crashed, or mixed up the instances' field and parameter types
- Fixed: a generic member called on two instances of one type constructor (`Box(u8).make(f)` and `Box(i64).make(f)`), or same-named generic members of unrelated types, shared one instantiation
- Add C types for talking to C, sized and signed by the target's C ABI: `c_char`, `c_short`, `c_ushort`, `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_longlong`, `c_ulonglong`, and `c_longdouble`
    - Each is its own type (`c_int` isn't `i32`, and `^c_int` doesn't convert to `^i32`); a value converts implicitly only where every value fits on the target, so `c_long` to `i32` compiles only where `c_long` is 32-bit
    - Arithmetic stays ghoti's: no C integer promotions
- `@cfg(testing)` and `@cfgValue(testing)` are true while building a test executable (`ghoti test`), so test-only helpers can sit next to the code they test
    - **Breaking:** `test` blocks are only checked in a test build; `build-*` and `run` no longer report errors inside them
- A `@cfg` / `@cfgValue` predicate can read another module's `pub` `@cfgValue` constant through its imports (`@cfg (std.os.HAS_BACKEND) { ... }`), so a gate shared across modules is written once
- A runtime argument to a parameter that a `const` function infers `comptime` names where the body reads it at compile time, and suggests `let` when that read is a `const` initializer (#365)
- Fixed: a statement starting with `extern struct`, `extern union`, or `extern fn`, like a `match` arm yielding `.little => extern struct { ... }`, was parsed as an `extern` declaration (#366)
- Fixed: a raw identifier spelling a primitive (`const @"f128" = struct { ... }`) took over the primitive's name, so `f128` meant the struct inside its own module, while `@"u8"` named the primitive instead of the declaration
- `@[testing]` marks a test helper: it may use `@expect`, `@require`, and `@skip`, so a check that takes more than one expression can live in a function instead of returning a `bool`
    - A `@require` or `@skip` in a helper stops the test that called it, through any helpers in between
    - A helper is only called directly, from a test or another `@[testing]` function
- `f80` works on every target, not only x86
    - Elsewhere it is stored as an 80-bit integer and its arithmetic, comparisons, conversions, and math builtins call compiler_rt routines named as Zig names them (`__addxf3`, `__ltxf2`, `__extenddfxf2`, `__floatdixf`, `__fixxfti`, `__sqrtx`, ...), each taking and returning the value's bits
    - Its layout matches x86's (16 bytes, 16-aligned) in structs, arrays, and closures on every target
    - `f80` constants fold at compile time identically on every target
    - Off x86 it has no C ABI, so `extern` declarations can't use it
- Fixed: the LSP could crash while a standard library file was mid-edit, when a file reached the broken module only through an import cycle (`std` -> `heap` -> `std`); a root that reaches a failed module is no longer lowered (#368)
- Fixed: an untyped constant bound to a name (`const n = 24;`, or a call to a function returning `comptime_int`) took a fixed `i32`/`usize` type next to a typed operand, so `x >> n` and `x + n` with `x: u64` were rejected; it now takes the other operand's type like a literal written in its place (#372)
- Fixed: an error found while collecting a generic's body (a shadowing `let`) was replaced by the `Failed to instantiate` it caused, so only the latter was reported (#370)
- Fixed: a parameter type reflecting on an earlier type parameter (`bits: ^mut @Int(.{ .bits = @typeInfo(T).float.bits, ... })`, `[@typeInfo(T).float.bits / 8]u8`) was evaluated before `T` was bound and reported an inactive union field (#371)
- Fixed: a non-test build crashed on a file whose `test` block imports a module (`test { import "x.gh"; }`), since that module is never resolved outside a test build
- Fixed: same-named generic functions in different modules (two files' private `helper`s) shared one instance per argument list, so a call could run the other module's body

## Standard Library
- Add `std.math.min` / `std.math.max` over two or more values
    - Both can be evaluated at compile time: `comptime { @assert(std.math.max(1, 2) == 2); }`
- `std.io.Reader`, `std.io.Writer`, and `std.io.Seeker` default their `Error` to `std.io.Error`, so `&mut dyn std.io.Writer` no longer needs `(Error = std.io.Error)`
- Add `std.meta` with `return_type(T)`, the return type of a function, closure, or erased `fn` type, directly or through `^` / `&`
- `std.Result.map` and `map_err` take an `impl Fn(...)`, so passing a function of the wrong shape is reported at your call instead of inside `std`
    - Their return types are written out (`Result(std.meta.return_type(@TypeOf(func)), E)`) rather than `auto`
- Fixed: `std.mem.Allocator.destroy` freed the size and alignment of the pointer rather than of what it points to; it takes `^mut auto` now
- Fixed: a skipped test printed `Test failed: <file>: (SKIPP ED)<message>`; it prints `Test skipped: <file>: (SKIPPED) <message>`
- **Breaking:** `std.mem.Allocator.free` takes just the slice (`allocator.free(buf)`); the element type comes from the slice instead of a separate `T` argument (#278)
- `import std;` works on every target, including ones std has no OS backend for (wasm, freestanding, Linux beyond x86_64 and aarch64)
    - There `std.os` is empty, and the parts built on it are left out: `std.io.File` and the standard streams, `std.heap`'s page allocator, `std.mem.round_up_page`, the test runner, and std's panic and assert handlers (the builtin defaults trap instead)
    - `std.os.HAS_BACKEND` says whether the target has one

## Tooling
- Lexing is faster: the longest operator's length is computed once instead of on every operator read, which made parsing a large file several times slower than it needed to be
- **Breaking:** `-M, --mode debug|release_safe|release_fast|release_small` replaces `-O`, `--release`, and `--unsafe` on `build-*`, `run`, and `test`
    - `debug` (the default) is `-O0` with runtime safety, `release_safe` is `-O2` with safety, `release_fast` is `-O3` without, and `release_small` is `-Oz` without
    - The LSP analyzes as `debug`
- Releases ship a `lib/compiler_rt` directory of ghoti sources for the routines LLVM calls on its own (`__addtf3`, `__divti3`, `fmodf`, ...)
    - A link whose object needs one builds `lib/compiler_rt` for the target (at most once per process) and links it last; `GHOTI_COMPILER_RT=<file>` points at another root
    - `--no-compiler-rt` on `build-exe`, `build-lib`, `run`, and `test` skips it
    - A routine that fails to compile, or that compiles into a call to itself, is a build error naming its file
    - A link that fails on a missing builtin says which ones and that `lib/compiler_rt` doesn't provide them yet
    - `lib/compiler_rt` can `import std;` like any program
- Fixed: a `build-lib --dynamic` DLL for an MSVC target exported nothing; Windows DLLs now export every non-hidden symbol, like `.so` and `.dylib`
- **Breaking:** a library built with `build-lib` keeps only its `export`ed symbols visible; everything else it holds, such as its copy of `std`, is internal so it can't collide with the program linking it, and a `panic_handler` / `assert_handler` it carries is weak so the program's own wins
- Fixed: `ghoti fmt` moved a comment between an `if`'s closing brace and its `else` into the `else` block, and one trailing the brace (`} // note`) into the block before it (#369)
- LSP completes attribute names inside `@[...]` and enum arguments like `@[visibility(.hidden)]`, and hover describes an attribute
- Fixed: the LSP kept workspace symbols and diagnostics for files that disappeared, such as ones on an unmounted drive, and logged a failed reanalysis for each of them on every edit
- Fixed: `ghoti fmt` left a line past 100 columns when what followed a group on that line (like `): void {` after a parameter list) pushed it over; a trailing comment still never forces a break
- Cross compiling for macOS links from any host: releases ship `libSystem.tbd` and `SDKSettings.json` in `lib/darwin`, used when neither `SDKROOT` nor `xcrun` names an SDK (#342)
    - The linker stamps the SDK version from `SDKSettings.json` into the image instead of reusing the minimum OS version
- LSP hover names a callable's parameters: `fn(lhs: i32, rhs: i32): i32` instead of `fn(i32, i32): i32` (#305)
    - Covers function declarations, `fn`-typed parameters and fields, `dyn Fn` aliases, and aliases like `f: Callback`, `f: mod.Callback`, or `const g := mod.f;`, including across modules
- Building from source: editing any header or `.inc` now always rebuilds the objects that include it
    - Zig 0.16 drops a cached object's headers from the library's cache manifest, so edits to them were silently ignored; the header stamp now comes from stdx, whose `zig build verify-deps` checks it against a fixture
    - `zig build -Dinstall-tests-only=true` now installs the test binaries instead of doing nothing
    - `zig build prune` deletes superseded `.zig-cache` generations and stale `zig-out` files, never LLVM unless `-Dprune-protected=true` (`-Dprune-dry-run=true` to preview)
        - Paths compare case-insensitively only on Windows and macOS, and either separator is accepted on every host
