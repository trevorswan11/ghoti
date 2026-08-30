# v0.1.0
- Initial release

# v0.2.0

## Testing
- `test` blocks now compile and run: `ghoti test` command, per-test functions, and test metadata (`builtin::Test`)
- `@expect` / `@require` assertions and `@skip()` (rejected outside a `test` block)
- Weak `test_runner` / `panic_handler` / `expect_handler` / `require_handler` / `skip_handler` hooks in the `builtin` module for running & failure/skip reporting

## Language
- `constexpr` function parameters
- Postfix `?` / `!` unwrap operators for `Result` / `Optional`
    - Requires any tagged union in the shape `union { .ok/.some: ..., .err/.none:... }`
- `if constexpr`: dead branches are no longer resolved or emitted, with per-instantiation pruning
- `match` on a compile-time `type`: dispatches on exact type identity
    - `if constexpr`-style (only the selected arm is checked/emitted)
    - Requires a `_` arm
    - Works per generic `T: type` instantiation
- Compile-time config: `@cfg`, `@cfgValue`, `@compileError`; target facts as `builtin` enums; `@cfg` can gate aggregate fields and nested control flow
- Inline assembly: `asm { ... }` expressions
- Declaration modifiers: `weak`, `naked`, `threadlocal`, and link-name overrides
- `undefined` for uninitialized bindings; required for non-`extern` uninitialized values
- `return` accepts implicit initializers (`return .{ ... }` / `return .variant`) through `if` / `match` / labeled `break`
- Module-scope `var` globals and `var` / `const` static members are first-class: bare-name / `Type.X` / `@this().X` read, address-of, and assignment; member functions usable as `fn` pointers
- Non-exhaustive enums: casting an integer to an enum without `_` is range-checked at runtime and panics on an unlisted value

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
