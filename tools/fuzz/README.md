# Compiler fuzzing

Two fuzzers, for two kinds of bug.

## Crashes: `smoke.py`

A mutation fuzzer. It harvests programs from the compiler tests' raw strings and `examples/`,
applies random token-level mutations (truncate, delete, duplicate, swap, splice, or insert a
keyword), and runs `ghoti build-obj` on each one. A run passes when ghoti exits cleanly or reports
a compile error. A crash, an assertion, an LLVM verifier error, a timeout, or a failing exit with no
diagnostic is a bug.

```
zig build
python tools/fuzz/smoke.py 20
```

The argument is the time budget in minutes. Failures are grouped by crash site, and each group
lists up to four reproducers. Cases go to `$TMP/ghoti-fuzz` (override with `GHOTI_FUZZ_CASES`), and
passing ones are deleted as they finish, so only reproducers stay. Delete the directory when you're
done with them. `GHOTI` points at a ghoti binary other than `zig-out/bin/ghoti`.

`harness.py <module> [command]` runs a fixed set instead: `<module>` is any importable module with
a `cases()` function that returns `(name, source)` pairs.

Every fixed crash gets a regression case in
`lib/compiler/tests/e2e/test_malformed_regressions.cc`.

## Wrong answers: `zig build fuzz-semantics`

The differential harness (`lib/compiler/tests/helpers/differential.{hh,cc}`) checks that every
integer and float operation folds at compile time to the bit pattern it computes at runtime, and
that every compile-time error is a runtime panic. It comes in three tiers:
- `zig build test-compiler` runs a few-minute slice: integer types up to 64 bits (plus the wide
  ones in families that never need compiler_rt), a trimmed boundary set, and 8 seeded random cases
  per operation.
- `zig build test-semantics` runs every type, the full boundary set, and 32 random cases. It takes
  well over half an hour; run it before a release.
- `zig build fuzz-semantics` (below) keeps drawing fresh random cases for a time budget.

`zig build fuzz-semantics -Dfuzz-minutes=20` repeats every operation family with fresh random seeds
until the budget runs out. A failure names its round's seed, and
`GHOTI_DIFF_FUZZ_SEED=<n> zig-out/tests/compiler "[.fuzz-semantics]"` replays that seed through
every family once.

Environment knobs for the regular differential tests:
- `GHOTI_DIFF_FULL=1`: every boundary value and more random cases per operation
- `GHOTI_DIFF_SEED=<n>`, `GHOTI_DIFF_ITERS=<n>`: the random cases' seed and count
- `GHOTI_DIFF_DUMP=<path>`: keep the last generated program, for reproducing a failure
