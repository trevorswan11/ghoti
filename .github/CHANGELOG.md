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
