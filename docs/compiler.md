# Compiler driver

The compiler driver provides the shared source-file frontend pipeline used by
`coglet` and semantic test tools.

Its current responsibilities are:

1. read one source file;
2. create the main and scratch arenas;
3. initialize the parser;
4. parse the program;
5. report parser diagnostics;
6. run semantic analysis;
7. report the semantic error summary;
8. retain frontend state for the caller until explicit destruction.

The driver does not perform token or AST snapshot dumping. `dump_tokens` remains
lexer-only and `dump_ast` remains parser-only.

## Ownership

`CompileResult` owns:

- the source buffer;
- the main arena;
- the scratch arena.

`CompileResult` borrows:

- the filename string.

The following data is backed by the owned arenas:

- AST nodes;
- parser diagnostics and diagnostic messages;
- semantic symbols and scopes;
- semantic types;
- expression side-table information.

The parser and semantic contexts are embedded in `CompileResult`, but they do
not independently own their arena-backed data.

All frontend state remains valid until:

```c
compile_result_destroy(&result);
```

After destruction, the source, AST, parser diagnostics, and semantic data must
not be accessed.

## Failure States

A result may be destroyed after every driver return status.

* COMPILE_STATUS_OK: parsing and semantic analysis succeeded.
* COMPILE_STATUS_SEMANTIC_ERROR: parsing succeeded; semantic state may be
  partial.
* COMPILE_STATUS_PARSE_ERROR: parsing failed; semantic analysis was not run.
* COMPILE_STATUS_DRIVER_ERROR: the frontend pipeline could not be started,
  such as when the source file could not be read.

Parser and driver errors map to process exit code 2. Semantic errors map to
exit code 1.

## Diagnostics

Parser diagnostics are accumulated by the parser and printed by the driver
after parsing fails.

Semantic diagnostics are printed immediately during semantic analysis. The
driver prints only the final semantic error-count summary.

Callers must not print these diagnostics again.


## Semantic Type Identity

Semantic analysis owns one canonical `Type *` for each concrete built-in
scalar and contextual built-in type:

```text
i8 i16 i32 i64
u8 u16 u32 u64
f32 f64
bool void null
```

Parsed scalar types are resolved to these shared instances. Pointer, array,
function, struct, enum, and untyped numeric types are not represented by one
generic canonical object because their structure or declaration identity
matters.

Type equality begins with pointer identity. Built-in scalars then compare by
kind, while pointers, arrays, and function types compare recursively.

Structs and enums are nominal: the semantic `Type *` allocated for the
declaration is its identity. Two different declarations remain different even
when they have the same source-level name, fields, members, or backing type.

The equality switch is exhaustive. A newly introduced `TypeKind` must define
its own equality rule rather than silently inheriting equality from a matching
kind.

Debug semantic-expression recording asserts that concrete built-in scalar
types use their canonical instances.

## Compile-Time Constant Evaluation

Integer constants use an exact sign-and-magnitude representation with a
`uint64_t` magnitude. Constant arithmetic selects a concrete provisional
operation kind, verifies operand representability, performs the mathematical
operation, and checks the result against both the operation type and any
untyped-integer domain limits.

Known integer division and remainder by zero are diagnosed both for fully
constant expressions and when only the divisor is compile-time known.
Compound `/=` and `%=` use the same check.

Integer bitwise constant evaluation converts exact sign-and-magnitude values
to an explicitly sized unsigned bit pattern, performs `~`, `&`, `|`, or `^`,
and converts the result back. Signed behavior is therefore defined by Coglet's
fixed-width two's-complement model rather than host signed-integer operations.

Shift constant evaluation also operates on the width-limited unsigned bit
pattern. Left shift discards high bits, unsigned right shift zero-fills, and
signed right shift adds an explicit sign fill. The evaluator never relies on
host right shift of a negative signed integer.

Statically known shift counts are rejected when negative or greater than or
equal to the left operand's width. The same helper is shared by ordinary
shifts and `<<=`/`>>=`. Unknown runtime counts remain a future execution-layer
check.

Floating constants are stored as host `double` values, but `f32` operations
are performed at `float` precision before being retained in the constant
value. Constant evaluation preserves IEEE-754 infinity, NaN, and signed zero
for both `f32` and `f64`.

Floating comparisons use the host floating comparison operators directly.
They are not reduced to a three-way comparison because NaN is unordered:

```text
NaN == NaN  -> false
NaN != NaN  -> true
NaN < x     -> false
NaN <= x    -> false
NaN > x     -> false
NaN >= x    -> false
```

Integer conversion rejects non-finite floating values. Checked conversion to
`f32` also rejects a finite value outside the finite `f32` range.

The compiler itself must not be built with floating-point options such as
`-ffast-math` that discard IEEE-754 NaN, infinity, signed-zero, or comparison
semantics relied on by constant evaluation.


## Semantic-Information Verification

`check_semantic_info` uses the same compiler-driver frontend pipeline and then verifies the semantic side table after a successful analysis.

Normal verification:

```sh
check_semantic_info source.cog
```

Diagnostic source-order dump:

```sh
check_semantic_info --dump-semantic-info source.cog
```

The verifier checks completeness, duplicate and orphan entries, type/category invariants, symbol associations, and the rule that variables and parameters have concrete types.

A program that fails parsing or semantic analysis is not required to have a complete semantic side table. With `--dump-semantic-info`, a partial table may be printed for diagnosis; it is not passed through the successful-program completeness verifier.
