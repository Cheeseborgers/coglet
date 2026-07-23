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
- definite-assignment flow storage and temporary flow snapshots;
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

Known integer division and remainder failures are diagnosed both for fully
constant expressions and when the relevant failure can be proven from
compile-time-known operands.

The shared failure rules cover:

a zero divisor;
signed minimum divided by -1;
signed minimum remaindered by -1.

Compound /= and %= use the same rules.

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
shifts and <<=/>>=.

A runtime-dependent count remains a valid frontend expression, but the language
contract requires a future execution layer to trap when the count is outside
the same range. Counts must not be masked modulo the width.

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

## Runtime Scalar Contract and Frontend Ownership

Ordinary signed and unsigned integer arithmetic is checked in every build
mode. Addition, subtraction, multiplication, signed negation,
increment/decrement, and their compound forms require a representable result.

Known failures are compile-time diagnostics. Runtime-dependent failures are
accepted by the frontend and must trap in any future execution layer.

Integer division and remainder additionally require:

a nonzero divisor;
no signed-minimum and -1 overflow case.

Numeric cast is also checked. A known unrepresentable conversion is
diagnosed, while a runtime-dependent conversion remains well typed and
requires a future runtime check.

The semantic-expression side table does not need fields such as
requires_overflow_check or requires_checked_cast. The operation kind,
resolved operand types, result type, and cast destination already determine
the language-required check.

A future lowering layer can derive the required behavior directly:

integer +, -, *       -> checked arithmetic
signed unary -        -> checked negation
integer / and %       -> divisor and signed-overflow checks
integer shift         -> shift-count range check
numeric cast          -> checked conversion
bitwise operation     -> fixed-width bit-pattern operation

This keeps semantic facts backend-neutral and avoids duplicating policy in
mutable flags.

Explicit wrapping arithmetic and truncating conversion will use separate
builtin identities. They must not be represented by scattered source-name
comparisons throughout semantic analysis. Their eventual implementation
should use one exhaustive builtin classification and one central semantic
dispatch path.

The exact runtime trap mechanism remains outside frontend ownership. At the
language level, a trap means that the operation produces no result and normal
execution cannot continue.

## Definite Assignment and Control-Flow Analysis

Semantic analysis performs definite-assignment and reachability analysis while traversing function bodies. Flow-sensitive information is stored separately from lexical symbols so that one symbol may have different initialization states at different program points.

### Variable storage and flow identity

Every variable symbol has a `VariableStorage` classification:

* `VARIABLE_STORAGE_NONE`;
* `VARIABLE_STORAGE_GLOBAL`;
* `VARIABLE_STORAGE_LOCAL`;
* `VARIABLE_STORAGE_PARAMETER`.

Only locals and parameters participate in function-local definite-assignment analysis.

Each analyzed function receives a unique flow-owner ID. Locals and parameters receive monotonically increasing variable IDs within that owner. The complete flow identity of a tracked variable is therefore:

```text
(flow owner ID, variable ID)
```

Variable IDs restart from zero for each function, but flow-owner IDs are not reused during a semantic-analysis run.

IDs are also not reused after lexical scope exit. This keeps shadowed declarations and stale semantic references distinct even when their source-level names are identical.

Globals do not receive local flow IDs and are not tracked by this analysis.

### Flow state

`FlowState` contains:

* the current function's flow-owner ID;
* an arena-backed byte array of initialization flags;
* a tracked-slot prefix count;
* allocated capacity;
* a `reachable` flag.

The count is the active tracked-slot prefix, not necessarily the number of currently visible variables. Because variable IDs are not reused, variables from exited inner scopes may leave unused slots below the current high-water boundary.

Registering a local or parameter creates or exposes its slot and records its initial state:

* parameters begin initialized;
* locals with successful initializers begin initialized;
* locals without initializers begin uninitialized.

All flow queries and mutations assert that the symbol belongs to the active flow owner. This prevents one function from accidentally consulting another function's numerically identical variable slot.

### Scope lifetime

Each lexical `Scope` records the flow-slot count that was active when the scope was entered.

When the scope exits:

* the tracked-slot prefix is truncated to the recorded mark;
* removed initialization flags are cleared;
* the function's next variable ID is not rewound.

This prevents block-local variables from leaking into later branch merges while preserving stable symbol identity.

### Identifier uses and assignments

Identifier checking distinguishes ordinary reads from direct plain-assignment targets.

An ordinary read consults the active flow state. A tracked variable is rejected when its initialization slot is not set.

A direct target such as:

```c
value = 10;
```

does not read the previous value. After the target and right-hand side have both checked successfully and their types are compatible, the direct target's slot is marked initialized.

Nested assignment targets continue to check their component expressions normally:

```c
point.field = 10;
values[index] = 10;
*pointer = 10;
```

These writes do not mark the whole base variable initialized.

Compound assignment and increment/decrement use ordinary read checking because they consume the previous value before writing a replacement.

Taking the address of a tracked local also performs the ordinary initialization check.

A malformed or type-invalid assignment does not update definite-assignment state.

### Flow cloning and merging

Branches and switch cases require independent flow states. `flow_clone()` therefore allocates and copies the initialization array rather than copying only its pointer.

Every clone preserves its flow-owner ID.

Flow merging first restricts both inputs to the tracked-slot prefix that was active before the branch. It then handles reachability as follows:

* when both paths continue, initialization flags are intersected;
* when only one path continues, that path is preserved;
* when neither path continues, the result remains unreachable.

An unreachable path therefore does not weaken a reachable path.

Merge helpers assert that both inputs belong to the same flow owner.

### Conditional statements

An `if` condition is checked before the incoming flow state is cloned.

The `then` and `else` branches each begin from independent copies of that state. When no explicit `else` exists, the false path is represented by an unchanged copy of the incoming state.

After both branches have been checked, their continuing paths are merged.

### Switch statements

Every switch case begins from the same incoming flow state. Cases never inherit definite-assignment state from preceding cases.

Case expressions are checked and converted to the switch expression's type before their values are recorded. Only successfully validated `ConstValue` entries contribute to duplicate detection or exhaustiveness.

Switch exhaustiveness is value-based:

* `default` covers every possible value;
* a Boolean switch requires both `true` and `false`;
* an enum switch requires every distinct declared runtime value.

Enum aliases with the same backing value therefore require only one corresponding case.

Invalid case expressions cannot make a switch exhaustive.

When a switch is not exhaustive, control-flow analysis includes an implicit no-match path containing the unchanged incoming state.

Continuing case flows are merged using the same reachability-aware intersection operation used by `if`.

### Loop contexts

Loop analysis uses a stack of `LoopFlowContext` values. Each context stores:

* the active variable-slot prefix at loop entry;
* accumulated reachable `break` exit states;
* accumulated reachable `continue` iteration states;
* the parent loop context.

`break` and `continue` target the nearest context.

The source-language legality of `break` and `continue` is determined by `loop_depth`. Once legality has been established, `current_loop` is required as an asserted internal invariant.

Recording either statement adds the current path to the appropriate accumulator and then marks the active path unreachable.

For a `for` loop, flow is checked in runtime order:

1. condition;
2. body;
3. post expression.

Normal body fallthrough and accumulated `continue` paths are merged before the post expression is checked. `break` and `return` paths do not reach the post expression.

Coglet does not currently compute a loop fixed point. A loop that may terminate normally preserves the unchanged incoming state as a conservative possible exit. Initialization performed only during an iteration therefore cannot become definitely initialized after the loop.

A literal-true loop with no reachable `break` is handled specially and leaves the surrounding flow unreachable.

### Unified reachability

`FlowState.reachable` is the single source of truth for whether normal control flow can continue.

The same state controls:

* branch merging;
* switch continuation;
* `return`;
* `break`;
* `continue`;
* unreachable-statement diagnostics;
* non-void function fallthrough checking.

`return`, `break`, and `continue` mark the active path unreachable.

Block traversal reports an unreachable statement when it encounters a statement after the active path has become unreachable. The statement is still semantically checked so that unrelated semantic errors are not silently hidden.

A non-void function is rejected only when its final flow state remains reachable. This accepts both explicit returns and provably non-continuing bodies such as literal-true loops without reachable breaks.

The older separate return-analysis and unreachable-analysis helpers are no longer used.

### Nested functions

Each function body begins with:

* a fresh `FlowState`;
* a new flow-owner ID;
* a variable-ID counter reset to zero;
* no inherited active loop;
* its own current return type.

Before entering a nested function, semantic analysis saves the enclosing function's:

* flow state;
* variable-ID counter;
* loop context;
* loop depth;
* return type.

These values are restored after the nested body has been checked. The global next-flow-owner counter is not restored, ensuring that every function receives a distinct owner.

Nested functions do not currently implement closure environments. A reference to an enclosing local or parameter is therefore rejected before definite-assignment state is queried.

Visible globals, constants, types, and function declarations remain accessible because they do not require an enclosing function's runtime flow slots.


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

The verifier checks completeness, duplicate and orphan entries, type/category invariants, 
symbol associations, and the rule that variables and parameters have concrete types.

Expression facts may be recorded before a later flow-sensitive use is rejected. 
For example, an identifier read can retain its resolved type, symbol, and lvalue category 
even when definite-assignment analysis subsequently reports that the variable may be uninitialized. 
The side table records successfully resolved expression facts; 
it is not itself a record that every later semantic rule accepted the expression's use.

A program that fails parsing or semantic analysis is not required to have a complete semantic side table. 
With `--dump-semantic-info`, a partial table may be printed for diagnosis; 
it is not passed through the successful-program completeness verifier.
