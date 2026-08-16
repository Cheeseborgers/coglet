# Compiler driver

The compiler driver provides the shared source-file frontend pipeline used by
`coglet` and semantic test tools.

Its current responsibilities are:

1. read the primary source file;
2. create the main and scratch arenas;
3. initialize a compilation-level `SourceManager` and register the primary file;
4. initialize the parser against that registered source file;
5. parse the program;
6. report collected parser diagnostics;
7. run semantic analysis against the same source manager;
8. report collected semantic diagnostics and the semantic error summary;
9. retain frontend state for the caller until explicit destruction.

The driver does not perform token or AST snapshot dumping. `dump_tokens` remains
lexer-only and `dump_ast` remains parser-only.


## Post-semantic IR boundary

The planned backend boundary is specified in `docs/ir.md`. Successful semantic
state lowers into an IR-owned typed CFG (`CogIR`), including an explicit ordered
module initializer for runtime-bearing program-scope code. Backends will consume
only the verified `CogIrModule`; they will not retain AST, `Symbol *`, frontend
`Type *`, or `SemanticContext *` dependencies.

## Ownership

`CompileResult` owns:

- the primary source buffer;
- the main arena;
- the scratch arena;
- compilation-level source-file metadata through `SourceManager`.

`CompileResult` borrows:

- the filename string.

The following data is backed by the owned arenas:

- source-file metadata and copied filenames;
- AST nodes and their `SourceSpan` provenance;
- parser/semantic diagnostics and diagnostic messages;
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
exit code 1. The `coglet` command uses exit code 3 when an explicitly requested
backend emission/link step fails after successful frontend checking.

## Source provenance and diagnostics

Source identity is compilation-local and multi-file-capable. `SourceManager`
registers each source buffer under a stable `SourceFileId`; the current driver
registers one primary file, while future module/import loading can register
additional files in the same manager without changing AST or diagnostic
representation. Source buffers must remain alive while their registered spans
are in use.

Tokens and AST nodes carry `SourceSpan` values containing:

- the source-file ID;
- start/end byte offsets;
- cached one-based start line and column.

Byte offsets are the canonical range for source slicing and highlighting. The
line/column cache makes common diagnostic rendering cheap. Composite expression
spans are formed from their child/token spans; declaration spans begin at the
declaration name, and statement spans begin at the controlling keyword or
expression.

Parser and semantic diagnostics use the shared `Diagnostic`/`DiagnosticList`
representation. Semantic analysis no longer writes individual errors directly
to `stderr`; it records them with a source span and the driver renders them after
checking. A typical diagnostic is:

```text
foo.cog:12:12: error: array index out of bounds
    return values[3];
           ^~~~~~~~
```

The generic diagnostic phase enum already reserves lexer, parser, semantic, IR,
and backend phases so CogIR lowering/verifier diagnostics can reuse the same
source provenance later. Parser and semantic error-count summaries remain phase
specific.


## External C Function Declarations

`#extern(c)` is represented by the ordinary `NODE_FUNC_DECL` AST node with
`FUNCTION_LINKAGE_EXTERN_C`. Its `body` is `NULL`; ordinary Coglet functions use
`FUNCTION_LINKAGE_COGLET` and retain a block body. `#repr(c)` on a Coglet-defined
function sets `NODE_FUNC_DECL.is_repr_c`; the function keeps its Coglet body but
its semantic `TYPE_FUNCTION` carries `FUNCTION_ABI_C` so it may cross a native C
callback boundary. Unannotated Coglet functions carry `FUNCTION_ABI_COGLET`. Native C function
types additionally carry a `CCallingConvention`; `C_CALL_DEFAULT` means the
platform/default C ABI, while explicit `cdecl`, `stdcall`, `sysv64`, and `win64`
contracts remain part of structural function-type identity.

The parser keeps `extern`, `c`, and option names such as `name` as ordinary
identifiers. Only `#` is new punctuation. This means the annotation syntax does
not consume globally useful identifier names and can coexist with future
declaration metadata such as `#repr(c)`.

`NODE_FUNC_DECL.external_name` stores an optional decoded external symbol name.
An empty view means the source-level Coglet function name is also the external
symbol. `#extern(c, name="...")` populates the override; the parser rejects an
empty name, invalid string escape, embedded NUL, duplicate `name`, or unknown
option. `NODE_FUNC_DECL.c_call_conv` stores the optional explicit C calling
convention for both extern declarations and `#repr(c)` callback definitions. AST
cloning preserves this metadata.

Semantic analysis registers external function signatures in the same function
namespace as ordinary functions, so duplicate-declaration and call-resolution
rules remain shared. Function symbols retain their source declaration node when
syntax-level metadata is semantically significant after type aliases have been
resolved. The first use is C-string binding: the checker can require that an
extern parameter was actually spelled `readonly c_char*`, rather than treating
every representation-equivalent `readonly i8*`/`readonly u8*` as a C string.
Body checking is skipped for external declarations.

The current C-ABI eligibility check permits concrete scalar types, raw typed
pointers recursively over supported pointees, opaque raw pointers, explicitly
`#repr(c)` structs/unions/enums, native C function-pointer types (`cfn(...) -> T`), and
`void` returns. Callback signatures are checked recursively against the same
subset. Arrays, ordinary Coglet structs/enums, and ordinary Coglet function
types remain rejected. External declarations are restricted to top level and
parameter defaults are rejected. `#repr(c)` functions are likewise top-level,
reject defaults, and must have a C-compatible signature.

Aggregate `#repr(c)` metadata is stored on `NODE_STRUCT_DECL` / `NODE_ENUM_DECL`
and propagated to semantic `TYPE_STRUCT` / `TYPE_ENUM`; represented unions reuse
`NODE_STRUCT_DECL` / `TYPE_STRUCT` field storage with an explicit union-layout
flag. Function `#repr(c)` metadata is stored separately on `NODE_FUNC_DECL` as
described above.
C-represented aggregates are top-level only. A `#repr(c)` enum requires an explicit
native C integer alias as its backing
type; the existing enum member range checks then apply to that resolved type.
The enum remains closed and nominal. The host-C backend represents it with a
typedef of the exact selected C integer spelling, avoiding C99's implementation-
defined native-enum underlying representation.

C-represented structs and unions are top-level only. Fields may use
scalar/raw-pointer ABI types, native C function pointers, pointers to other
represented aggregates, complete `#repr(c)` structs/unions directly by value, or
positive-length fixed arrays of any supported field type. Semantic analysis
rejects direct and indirect by-value layout cycles, including cycles reached
through array elements, while allowing recursive pointer graphs. Empty
structs/unions, unsized/zero-length arrays, ordinary enums, and ordinary structs
remain rejected; `#repr(c)` enums and `cfn` values are valid field/array element
types. The host-C backend assigns generated native C struct/union tags, emits
forward typedefs before pointer aliases, and topologically emits complete
aggregate definitions so by-value dependencies do not depend on Coglet source
order. Fixed array fields are emitted as native C array declarators; fields
otherwise remain in source order and use source-level C ABI spellings. Arrays are
still rejected in `#extern(c)` parameters and returns. Complete represented
structs/unions may also carry `packed` and/or `align=N` metadata. Semantic
analysis requires `N` to be a positive power of two and rejects layout controls
on incomplete structs. The host-C backend lowers these controls through guarded
GNU-compatible `__attribute__((packed))` / `__attribute__((aligned(N)))`
annotations; generated C fails explicitly on host compilers without that
attribute model rather than silently using the wrong ABI layout. `align=N` is a
minimum alignment request, while `packed` controls member placement. Direct
Coglet-side union member access/construction is deliberately rejected until
active-member semantics are specified.

A body-less `#repr(c) Name::struct;` declaration is represented as a nominal
`TYPE_STRUCT` marked incomplete. Semantic analysis permits that type only behind
raw pointers; all by-value storage, dereference/index operations, construction,
and field access are rejected. In the C backend the type participates in the
same generated struct registry as complete represented structs, but only its
forward typedef is emitted. This is sufficient for ABI-correct opaque C handles
such as `SDL_Window *` without importing the foreign layout into Coglet.

Semantic startup registers the native C scalar family as builtin type aliases:
`c_char`, `c_schar`, `c_uchar`, `c_short`, `c_ushort`, `c_int`, `c_uint`,
`c_long`, `c_ulong`, `c_longlong`, `c_ulonglong`, `c_size`, `c_bool`, `c_float`,
and `c_double`. Their semantic mappings now come exclusively from `TargetInfo`,
not from `sizeof`/`CHAR_MIN` inside semantic analysis. The target description
supplies pointer width, C integer widths, plain-`char` signedness, `_Bool` width,
and C floating formats. `c_bool` resolves to canonical Coglet `bool`; `c_float`
and `c_double` are admitted only when the selected target reports IEEE
binary32/binary64 respectively. This keeps type checking target-explicit while
preserving source-level ABI intent for backend emission.

Explicit C calling conventions are lowered at the generated-C type/declaration
level. Callback typedefs carry the convention as part of the function-pointer
declarator, while extern declarations and `#repr(c)` function declarations/
definitions carry the same convention attribute. The current backend emits
guarded GNU/Clang-compatible mappings for `stdcall`, `sysv64`, and `win64` and
fails during generated-C compilation when the selected host architecture cannot
represent the requested convention; `cdecl` maps to the normal C convention
outside 32-bit x86. `stdcall` variadics are rejected semantically.

The **host-C backend** provides the bootstrap executable lowering path.
`coglet input.cog -o program` emits a temporary C translation
unit, invokes the native `cc` driver, and lets that driver resolve normal C
runtime/libc symbols. External declarations are emitted under generated C
identifiers with `__asm__("symbol")` labels, so `name="..."` overrides affect
the actual linker symbol without requiring that symbol to be a safe C identifier.

The backend now consumes CogIR only and has explicit emission for every current
`CogIrOp`. The build mirrors that dependency direction: `compiler_core` contains
the frontend, semantic analysis, target description, and CogIR implementation,
while `coglet_backend_c` is a separate library that depends on the core. This
keeps backend dependencies out of frontend/IR-only tools. The optional LLVM
backend is now attached at exactly that boundary as `coglet_backend_llvm`; LLVM
headers/libraries are dependencies of that backend target only, not of
`compiler_core`.
Direct named calls,
locals, scalar globals/module initialization, checked and wrapping integer
arithmetic, checked-count shifts, floating arithmetic/comparisons, structured
control flow, field/array-field/pointer addressing, volatile scalar loads/stores,
and checked/truncating/raw-pointer casts execute from verifier-checked IR. Native-C
variadic promotions are explicit CogIR operations rather than an implicit host-C
side effect. First-class `cfn` values preserve their exact recursive C ABI spelling
through parameters, locals, loads, CFG spills, and C-call results, so indirect
calls use the same verifier-owned ABI contract as direct extern calls, including
variadic callbacks. String escapes are decoded using Coglet's literal rules and
lowered through IR-owned backing data at supported C ABI boundaries. Host
executables require a source-top-level `main::() -> i32`. Semantic analysis
validates that source contract before C aliases are canonicalized, and lowering
records only the resolved function identity as `module.entry_function`. The
backend therefore selects entry by IR identity rather than scanning debug names
after nested functions have been flattened. The host-C backend adapts the Coglet
`i32` result to the C process ABI in its generated `int main(void)` wrapper.
Aggregate values
now execute through assignable C
wrapper structs for Coglet arrays while addressable array storage remains native C
arrays, preserving represented aggregate layout and by-value Coglet semantics.
Struct/array construction and extraction, aggregate globals, aggregate arguments
and returns, and represented aggregate C interop all use that same IR-owned
representation boundary. Compiler-generated strings now share the general array
storage path rather than requiring a backend-only string special case. Checked
operations are not lowered through C behavior that would introduce undefined or
implementation-defined semantics where Coglet requires a trap or fixed-width result.

`coglet input.cog --emit-c generated.c` exposes the generated translation unit
for inspection. Running `coglet input.cog` without `-o` or `--emit-c` preserves
the previous frontend-only parse/check behavior.

Executable linking accepts repeated native library search paths and libraries in
both conventional forms:

```text
-L/path/to/lib     -L /path/to/lib
-lfoo              -l foo
```

The driver preserves library order, places explicit libraries after the
generated C source in the native compiler invocation, and passes every argument
directly through `execvp()` rather than constructing a shell command. `-L` and
`-l` are rejected unless `-o` requests an executable link step. `--emit-c`
remains independent of linker options.

The frontend now has an explicit target model. `compile_parse_and_check()`
constructs a host `TargetInfo` for compatibility, while
`compile_parse_and_check_for_target()` accepts an explicit description and
copies it into `CompileResult`/`SemanticContext`. Synthetic-target tests compile
the same source under different C scalar mappings to ensure semantic analysis
is not consulting the build host. CLI target selection, target triples,
backend-specific data layout, cross toolchain selection, and actual cross-linking
remain deferred; the current C backend is still intentionally a host backend and rejects semantic state whose `TargetInfo` does not exactly match the build host.
Native Coglet variadics, richer callback lifetime/closure machinery, custom
native compiler selection, raw linker flags, and non-C calling-convention
details also remain deferred.

## Target Description


### LLVM backend Stage 6

The LLVM backend lives under `src/backends/llvm/` with its public API under
`include/backends/llvm/`. It accepts only frozen `CogIrModule` input, constructs
LLVM IR through the LLVM C API, runs `LLVMVerifyModule`, and then writes textual
IR. Stage 6 retains the native execution foundation from Stages 1-4 and the
Stage 5 scalar/pointer/function C ABI surface, then adds target-aware C object
storage and represented aggregate classification. Native C scalar aliases, raw
pointers, incomplete handles, callbacks, variadics/default promotions, external
symbols, and explicit calling conventions continue to derive only from frozen
CogIR metadata. Complete `#repr(c)` structs/unions now use backend-owned physical
storage types and, on x86-64, are classified for SysV or Win64 calls as direct
register components or indirect `byval`/hidden-`sret` values as required.

Before lowering types, the backend initializes LLVM's native target, creates a
target machine, and stamps the module with the resulting target triple and
`DataLayout`. These LLVM-specific objects remain backend-owned; they are not
added to CogIR or frontend `TargetInfo`. The LLVM backend remains host-target-only
until Coglet has explicit target selection and cross-toolchain policy.

Runtime integer failure paths branch to `llvm.trap`; checked overflow uses LLVM's
`*.with.overflow` intrinsics rather than relying on poison-producing flags or
host-language behavior. Floating operations are emitted without fast-math flags.
Comparisons use predicates that preserve Coglet's specified NaN behavior, and
checked floating/integer conversions guard non-finite and range failures before
the LLVM conversion instruction executes. Backend-created trap/continuation
blocks preserve CogIR block-parameter semantics by recording the actual current
LLVM predecessor when adding PHI inputs.

Native Coglet function *values* lower as ordinary LLVM opaque pointer values.
Their callable signature is derived separately from the frozen CogIR function
type when a function is declared or called. This distinction permits native
function values to pass through locals, parameters, results, and CFG storage
without treating an LLVM function type itself as a first-class data value. C function values use the same opaque pointer value representation, but their
callable signatures and call-site attributes are derived from CogIR's exact C ABI
type metadata rather than from the ordinary runtime type alone.

CogIR `switch` terminators lower to LLVM switch control flow. Per-edge trampoline
blocks preserve edge-specific CogIR block arguments even when more than one case
targets the same destination block. CogIR `trap` terminators lower explicitly to
`llvm.trap` followed by `unreachable`.

Stage 6 distinguishes LLVM runtime values from C object storage. In particular,
scalar C `_Bool` call values remain logical `i1`, while addressable `c_bool`
objects use the target C `_Bool` storage width and are converted on load/store.
The same conversion is recursive through represented C array/struct fields.
Exact object spelling is frozen on CogIR slots/globals/addresses where canonical
semantic identity would otherwise lose this distinction. Passing an ordinary
Coglet `bool*` as `c_bool*` is rejected rather than silently reinterpreting an
`i1` object as C storage.

Represented aggregate classification is currently implemented for x86-64 SysV
and Win64. SysV values are classified into INTEGER/SSE eightbytes or MEMORY;
Win64 uses its size-based direct/indirect aggregate rules. Packed or misaligned
SysV objects fall back to MEMORY, and explicit aggregate alignment is reflected
in LLVM storage plus `sret`/`byval` alignment attributes. This is a backend ABI
plan, not an ordinary LLVM-struct shortcut: the same plan drives declarations,
call sites, C callback definitions, parameter reconstruction, and returns.
Volatile whole-aggregate accesses, optimization pipelines, object emission,
linker integration, and non-x86-64 represented aggregate classification remain
explicitly deferred.

`COGLET_LLVM=AUTO` enables the backend when `LLVMConfig.cmake` is available;
`ON` requires LLVM 17 or newer and `OFF` disables it. The CMake integration
supports both monolithic LLVM shared-library packages and component-library
installations.

`TargetInfo` is the backend-neutral target contract needed by the frontend. Its
fields are expressed in bits and currently cover:

- pointer width;
- C `char` width and signedness;
- C `_Bool`, `short`, `int`, `long`, `long long`, and `size_t` widths;
- C `float` and `double` format classification.

The host target constructor is isolated in `src/target_info.c`; this is the only
frontend-support code that queries the C implementation used to build Coglet.
`semantic_anal.c` consumes only the copied `TargetInfo`. LLVM data layout,
register information, object format, relocation model, and calling-convention
lowering are intentionally not part of this structure. The LLVM backend
constructs its own target machine and `DataLayout` from the selected native LLVM
target without extending frontend object lifetimes or writing those backend facts
back into CogIR.

The structure is expected to grow only when frontend-visible target semantics
require additional backend-neutral facts; LLVM implementation details remain
owned by the LLVM backend.

## Semantic Type Identity

Semantic analysis owns one canonical `Type *` for each concrete built-in
scalar and contextual built-in type:

```text
i8 i16 i32 i64
u8 u16 u32 u64
f32 f64
bool void null
```

Parsed scalar types are resolved to these shared instances. Typed pointer,
opaque pointer, array, function, struct, enum, and untyped numeric types are not
represented by one generic canonical object because their structure,
permissions, or declaration identity matters.

Type equality begins with pointer identity. Built-in scalars then compare by
kind. Arrays and functions compare structurally. Function type equality also includes
`FunctionAbi`, so an ordinary Coglet function signature is distinct from an
otherwise identical native C callback signature. Pointer type equality includes exact pointee-type equality, exact `PointerAccess`, and the immediate volatile flag, so `T*`, `readonly T*`, `volatile T*`, and `readonly volatile T*` are distinct semantic types.

Opaque raw pointers use the dedicated `TYPE_OPAQUE_POINTER` kind rather than
`TYPE_POINTER(TYPE_OPAQUE)`. There is no standalone opaque value type. This
preserves the invariant that `TYPE_POINTER` always has a dereferenceable Coglet
pointee. Additional pointer layers compose normally, so `opaque**` is
`TYPE_POINTER` whose element is `TYPE_OPAQUE_POINTER`.

`TYPE_OPAQUE_POINTER` carries the same immediate `PointerAccess` permission and volatile flag as typed pointers. Its type identity includes both qualifiers, but it has no `element` pointee type.

Directional compatibility is separate from type identity. Immediate raw-pointer qualification may safely add `readonly`, `volatile`, or both:

```text
T*          -> readonly T*
T*          -> volatile T*
T*          -> readonly volatile T*
volatile T* -> readonly volatile T*
```

Neither qualifier may be removed implicitly. The immediate pointee types must already be exactly equal, preventing recursive qualifier insertion such as `T** -> volatile T**`.

Pointer equality comparison may ignore immediate readonly and volatile qualifier differences within the same pointer family. Typed pointers must
have exactly equal immediate pointee types; opaque pointers require no pointee
comparison. Typed and opaque raw pointers are not directly comparable.
Comparison does not alter either operand's permissions.

Typed/opaque crossings use `CAST_REINTERPRET`. Semantic checking requires one
side to be `TYPE_OPAQUE_POINTER` and the other to be `TYPE_POINTER`, and rejects any conversion that changes an immediate readonly source into a mutable target or discards an immediate volatile source qualifier.
The operation is intentionally not a general typed-pointer-to-typed-pointer
cast.

`SemExprInfo` also records whether an lvalue is volatile. Dereference and pointer indexing derive that bit from the pointer type; field/array subobjects inherit it from their containing lvalue; address-of copies it back onto the resulting pointer type. This preserves volatile access intent even before the native backend grows direct memory-operation lowering.

Structs and enums are nominal: the semantic `Type *` allocated for the
declaration is its identity. Two different declarations remain different even
when they have the same source-level name, fields, members, or backing type.

The equality switch is exhaustive. A newly introduced `TypeKind` must define
its own equality rule rather than silently inheriting equality from a matching
kind.

Debug semantic-expression recording asserts that concrete built-in scalar
types use their canonical instances.

## Compile-Time Constant Evaluation

Compile-time values now use one semantic `ConstValue` contract shared by
constant declarations, enum members, constant-expression checking, and later
lowering. The evaluator remains private to semantic analysis; successful
results are cached in semantic side tables while lexical scope is still live.
After `semantic_check()` returns, later phases retrieve them through:

```c
semantic_get_constant_value(ctx, node, &value)
```

The API accepts a checked constant expression, constant declaration, or enum
member declaration. It does not re-run name lookup or evaluation. Expression
results are normalized to `semantic_get_effective_expr_type()`, including
integer/float materialization and typed `null`, so lowering receives the exact
checked use-site representation. Constant declarations and enum members retain
their resolved declaration type.

`SemExprInfo` stores the intrinsic cached value, while `SemDeclInfo` stores the
final value for constant-like declarations. `Symbol` no longer duplicates the
constant payload: constant identifier evaluation follows the stable declaration
ID back to `SemDeclInfo`. Implicit enum members are recorded too, so later
phases do not have to reproduce enum auto-increment rules.

Fixed-array lengths are currently parsed as integer literal syntax rather than
general constant expressions, so there is no separate array-length evaluator
to migrate at this stage. Expanding array lengths to arbitrary constant
expressions would be a language feature, not part of this normalization.

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

Wrapping builtin evaluation operates on explicit fixed-width bit patterns.
`wrapping_add`, `wrapping_sub`, `wrapping_mul`, and `wrapping_neg` evaluate
modulo `2^N` for the concrete integer width.

Truncating integer conversion retains the low target-width bits of the exact
mathematical source value and reconstructs the result according to the target
signedness. The evaluator does not rely on host-C signed overflow or
implementation-defined narrowing conversions.

`NODE_CAST` constant evaluation dispatches exhaustively on `CastKind` so
checked and truncating conversions cannot accidentally share the wrong
conversion path.

## Runtime Scalar Contract and Frontend Ownership

Ordinary signed and unsigned integer arithmetic is checked in every build
mode. Addition, subtraction, multiplication, signed negation,
increment/decrement, and their compound forms require a representable result.

Known failures are compile-time diagnostics. Runtime-dependent failures are
accepted by the frontend and must trap in any future execution layer.

Integer division and remainder additionally require a nonzero divisor and no
signed-minimum/`-1` overflow case.

Numeric `cast` is checked. A known invalid conversion is diagnosed, while a
runtime-dependent conversion remains well typed and requires a future runtime
check.

Explicit wrapping arithmetic uses stable `BuiltinKind` identities registered
in the root scope. Calls pass through ordinary lexical resolution and one
central exhaustive builtin dispatcher. Semantic expression facts retain the
resolved builtin symbol rather than depending on source-name comparisons.

Truncating integer conversion is represented by `NODE_CAST` with
`CAST_TRUNCATING`, alongside checked conversion represented by `CAST_CHECKED`.
Opaque/typed raw-pointer conversion is represented by the same AST node with
`CAST_REINTERPRET`. Semantic checking dispatches exhaustively on `CastKind`;
constant evaluation explicitly rejects reinterpretation because it is not a
compile-time constant conversion.

A future lowering layer can derive required behavior directly:

```text
integer +, -, *       -> checked arithmetic
signed unary -        -> checked negation
integer / and %       -> divisor and signed-overflow checks
integer shift         -> shift-count range check
numeric cast          -> checked conversion
reinterpret            -> preserve address bits, change raw-pointer type
truncate              -> fixed-width low-bit integer conversion
wrapping builtin      -> fixed-width modulo integer arithmetic
bitwise operation     -> fixed-width bit-pattern operation
```

The semantic-expression side table does not need mutable flags such as
`requires_overflow_check`. Operation kind, resolved types, builtin identity,
and `CastKind` already determine the required behavior.

The exact runtime trap mechanism remains outside frontend ownership. At the
language level, a trap means the operation produces no result and normal
execution cannot continue.

## Storage Access Facts

Semantic expression information separates storage identity from write
permission:

```text
ValueCategory:
    NONE
    RVALUE
    LVALUE

ValueAccess:
    NONE
    READONLY
    WRITABLE
```

The valid combinations are:

```text
NONE    + NONE
RVALUE  + NONE
LVALUE  + READONLY
LVALUE  + WRITABLE
```

This distinction allows `*readonly_pointer` to identify storage while
remaining non-assignable.

Semantic checking propagates access as follows:

```text
dereference       <- pointer access
pointer index     <- pointer access
array index       <- array expression access
field access      <- object expression access
address-of result <- operand storage access
```

Assignment, compound assignment, increment, and decrement require a writable
lvalue. Address-of requires an lvalue but does not require writable access.

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

The parser normalizes an unbraced `if`/`else`/`while`/`for` body into a lexical
`NODE_BLOCK`, so later semantic and lowering phases see one scope model regardless
of source spelling. A parenthesized three-clause `for` initializer is normalized
into a surrounding lexical block containing the initializer followed by the
existing `NODE_FOR`; no initializer field or backend loop operation is required.

After that optional initializer has executed, a `for` loop is checked in runtime order:

1. condition;
2. body;
3. post expression.

Normal body fallthrough and accumulated `continue` paths are merged before the post expression is checked. `break` and `return` paths do not reach the post expression.

Coglet does not currently compute a loop fixed point. A loop that may terminate normally preserves the unchanged incoming state as a conservative possible exit. Initialization performed only during an iteration therefore cannot become definitely initialized after the loop.

A loop with a Boolean condition proven by the central constant evaluator to be `true` and no reachable `break` is handled specially and leaves the surrounding flow unreachable. The condition need not be the literal token `true`; named/local constants and checked constant Boolean expressions participate as well.

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

A non-void function is rejected only when its final flow state remains reachable. This accepts both explicit returns and provably non-continuing bodies such as compile-time-true loops without reachable breaks.

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

The nested function starts with loop depth zero and no current loop. Consequently, `break` or `continue` inside a nested function cannot target a loop in the enclosing function; each function owns an independent CFG/control-flow context.

These values are restored after the nested body has been checked. The global next-flow-owner counter is not restored, ensuring that every function receives a distinct owner.

Nested functions do not currently implement closure environments. A reference to an enclosing local or parameter is therefore rejected before definite-assignment state is queried.

Visible globals, constants, types, and function declarations remain accessible because they do not require an enclosing function's runtime flow slots.


## Semantic Declaration Identity

Successful source declarations now receive a stable semantic identity through
`SemDeclInfo`. The ID is unique within one `semantic_check()` invocation and is
independent of AST and `Symbol *` addresses. This gives later lowering stages a
reliable declaration key without repeating lexical name resolution.

Declaration metadata records:

- the declaration AST node;
- its resolved semantic type;
- its stable `SemDeclId`;
- its lexical `Symbol *` when the declaration introduces one.

Globals, locals, constants, functions, structs, enums, parameters, struct
fields, and enum members all receive declaration metadata after successful
resolution. Aggregate members do not acquire artificial lexical symbols.
Parameters in body-less declarations such as `#extern(c)` still receive an ID
and resolved type; parameters in a Coglet function body attach their lexical
symbol to that same existing declaration record.

Ordinary declared symbols mirror the declaration node and ID in
`Symbol.declaration` and `Symbol.declaration_id`. Compiler-provided builtin
symbols and native-C type aliases use `INVALID_SEM_DECL_ID`.

The public semantic API supports lookup by declaration node and by stable ID:

```c
semantic_get_decl_info(ctx, node);
semantic_get_decl_info_by_id(ctx, id);
```

The declaration IDs are deterministic for a given semantic traversal but are
compiler-internal identities, not persistent source/module IDs.

## Contextual Conversion Metadata

Semantic analysis now records implicit/contextual expression adaptation
directly in `SemExprInfo`. The expression's existing `type` remains its
intrinsic semantic type; when an enclosing use-site selects another concrete
type, the side table records both the destination and the reason:

```text
SemExprInfo.type                  intrinsic expression type
SemExprInfo.contextual_type       selected use-site type, or NULL
SemExprInfo.contextual_conversion adaptation kind
```

Current conversion kinds cover:

- adaptable `untyped-int` materialization to a concrete integer;
- adaptable `untyped-int` materialization directly to `f32`/`f64`;
- adaptable `untyped-float` materialization to `f32`/`f64`;
- `null` adaptation to a concrete typed/opaque raw pointer or native C function
  pointer;
- monotonic immediate raw-pointer qualification (`readonly`/`volatile`
  addition only);
- the deliberately narrow direct string-literal binding to
  `readonly c_char*` at supported C call boundaries.

The public helper:

```c
semantic_get_effective_expr_type(ctx, node)
```

returns `contextual_type` when a conversion was selected, otherwise the
intrinsic `type`. This is the API intended for CogIR lowering. Lowering should
not repeat literal-range checks, infer a default type for an untyped value, or
re-run pointer-qualification compatibility.

The metadata is recorded at variable/parameter/constant initialization,
assignment, returns, fixed call arguments, array/struct elements, numeric
operations and comparisons, switch labels, inferred numeric storage, array
indexes/shift counts that require default materialization, null comparisons,
and supported C literal boundaries.

Explicit casts are intentionally not represented as contextual conversions:
their conversion kind and destination already exist explicitly in the
`NODE_CAST` AST. Likewise, C default argument promotions for already-concrete
variadic values remain ABI-lowering rules rather than semantic contextual
conversions. Only otherwise-untyped variadic literals receive semantic
materialization; CogIR lowering now legalizes concrete variadic values with an
explicit non-trapping `c.vararg.promote` operation (`bool`/narrow integer or
`#repr(c)` enum -> native `c_int`, `f32` -> `f64`).

## Normalized ABI Declaration Metadata

Semantic analysis also normalizes declaration-level ABI contracts into
`SemDeclInfo`. Later lowering/backend phases therefore do not need to interpret
`#extern(c)` or `#repr(c)` syntax directly.

Function declarations record:

- Coglet versus native-C calling ABI;
- internal definition versus external linker declaration;
- the normalized C calling convention;
- C variadic status;
- the effective external linker symbol, with omitted `name=` normalized to the
  Coglet declaration name;
- a normalized C-facing return type for native-C ABI functions.

Represented aggregates record:

- Coglet versus C representation;
- struct versus union layout;
- incomplete status;
- packed layout;
- explicit minimum alignment.

Enums record Coglet versus C representation and, for `#repr(c)` enums, the
normalized native-C backing type.

C ABI parameters and `#repr(c)` aggregate fields carry a `SemAbiType`. This is
not a second semantic type system: every node in the ABI type tree points at its
already-resolved semantic `Type *`. The additional tree preserves only source
ABI spelling that canonical semantic resolution intentionally erases. For
example:

```text
source type        semantic type       normalized ABI spelling
-----------        -------------       -----------------------
c_int              i32                 C int
readonly c_char*   readonly i8*        pointer -> C char
cfn(c_int)->c_int  cfn(i32)->i32        cfn(C int) -> C int
```

Native-C scalar aliases are represented by `SemCScalarKind`; pointer, opaque
pointer, array, and C-function-pointer structure is retained recursively.
Nominal types and ordinary fixed-width Coglet scalars reuse their resolved
semantic type directly.

CogIR lowering consumes this normalized metadata for C linkage, calling
conventions, external symbols, represented aggregate layout, C enum backing
types, and C-facing function/field type spelling. The host-C backend now consumes
only those IR-owned copies; it does not inspect `SemDeclInfo`, semantic `Type *`,
or AST syntax. C-variadic default promotions are likewise explicit in CogIR before
the backend sees a call.

Executable entry spelling is handled before canonical semantic type resolution
erases the distinction between native `i32` and C aliases such as `c_int`. A
source-top-level `main` is required to be an ordinary Coglet `() -> i32`
function. CogIR then retains only the resolved `module.entry_function` identity
and verifies its backend-neutral `() -> i32` runtime signature. Ordinary Coglet
functions do not carry source C-scalar spelling metadata. Exact C spelling
continues to live only in normalized ABI metadata for genuine C boundaries such
as extern declarations, callbacks, represented fields, and C variadics. The
driver destroys `CompileResult` before calling the host-C backend, enforcing this
boundary in every `--emit-c` and `-o` compilation.

## Semantic-Information Verification

`check_semantic_info` uses the same compiler-driver frontend pipeline and then
verifies the semantic side tables after successful analysis.

Normal verification:

```sh
check_semantic_info source.cog
```

Diagnostic source-order dump:

```sh
check_semantic_info --dump-semantic-info source.cog
```

The verifier checks:

- completeness, duplicate entries, and orphan entries for expressions and declarations;
- stable declaration-ID uniqueness and reverse lookup;
- declaration/Symbol/type consistency;
- normalized function/aggregate/enum ABI metadata;
- recursive preservation of native-C scalar spellings at ABI surfaces;
- type, symbol, and value-category invariants;
- contextual conversion kind/destination consistency and effective-type lookup;
- cached constant-expression retrieval and effective constant type normalization;
- constant/enum-member declaration values through the centralized semantic API;
- valid `ValueCategory`/`ValueAccess` combinations;
- readonly and writable dereference propagation;
- pointer and array index access propagation;
- field-access inheritance;
- address-of access preservation;
- canonical built-in scalar types;
- concrete variable and parameter types;
- builtin identity and mutation-node invariants.

Expression facts may be recorded before a later flow-sensitive use is
rejected. For example, an identifier read can retain its resolved type, symbol,
and lvalue facts even when definite-assignment analysis subsequently reports
that the variable may be uninitialized. The side table records successfully
resolved expression facts; it is not itself a record that every later semantic
rule accepted the expression's use.

A program that fails parsing or semantic analysis is not required to have a
complete semantic side table. With `--dump-semantic-info`, a partial table may
be printed for diagnosis; it is not passed through successful-program
completeness verification.

CogIR lowering has two explicit stages. Metadata preparation copies source
provenance into IR ownership, maps resolved semantic/ABI types and compile-time
values, predeclares nominal types/functions, creates zero-initialized global
storage, and records `SemDeclId` -> CogIR declaration bindings. Executable
lowering now covers direct locals/globals, checked arithmetic, comparisons, typed
calls, assignment/compound mutation, `if`, short-circuit Boolean expressions,
`while`, `for`, nearest-loop `break`/`continue`, and non-fallthrough `switch` CFGs.
Ordered source global/top-level execution uses the same machinery inside the
synthetic module initializer. Values that must survive a short-circuit CFG split
are spilled to compiler-generated slots rather than relying on implicit dominance.
Executable lowering now also uses a generic place abstraction for identifier,
field, array-index, pointer-index, and dereference storage. Aggregate construction
and by-value flow, address-of/dereference, checked/truncating/reinterpret casts,
volatile loads/stores, character literals, fixed-array strings, and the direct C
string-literal boundary all lower into verifier-checked CogIR. Explicit casts
materialize adaptable numeric/null constants directly in their checked destination
type so frontend-only types never escape into IR. Runtime `wrapping_add`,
`wrapping_sub`, `wrapping_mul`, and `wrapping_neg` calls
now lower directly to `iadd.wrap`, `isub.wrap`, `imul.wrap`, and `ineg.wrap`;
compile-time wrapping calls continue to materialize through checked constant
metadata. All 100 programs under `tests/test_assets/semantic/valid/` now lower and
verify through `dump_ir`. Native-C variadic calls are legalized before the call:
CogIR inserts `c.vararg.promote` for target-required integer/Boolean/enum and
`f32` promotions, and the verifier rejects unpromoted C-variadic tails. The
host-C backend now consumes only a frozen `CogIrModule`. Its execution subset
includes the pre-port straight-line surface plus dedicated CogIR wrapping and
checked integer operations. Wrapping C emission performs the operation in unsigned
`uint64_t` bit-pattern space, masks narrower widths explicitly, and reconstructs
signed results without host signed overflow or implementation-defined narrowing.
Checked `+ - * / %` and signed negation test representability/divisor preconditions
before evaluating the host operation; failures realize the CogIR arithmetic trap
with standard C `abort()`. The backend also emits reachable multi-block CFGs as C
labels/gotos, including parallel block-parameter edge transfer, `br`, `cond_br`,
`switch`, `trap`, and `unreachable`. Integer predicates and Boolean negation support
that control-flow slice. Ordinary bitwise operations, checked-count shifts, `f32`/`f64` arithmetic and
comparisons, floating negation, and pointer equality/inequality also execute
through CogIR without falling back to frontend semantics. Runtime shifts guard
`0 <= count < width` before any C shift; left shift operates on fixed-width
unsigned bit patterns and signed right shift synthesizes sign fill explicitly, so
the backend does not depend on implementation-defined right shift of negative C
integers. Scalar source globals
plus the ordered module-init function make top-level runtime control flow executable
through the same IR-only path. The data/address execution slice now also emits
`field_addr` for represented structs/unions, `array_elem_addr` through represented
array fields, and `ptr_index_addr` for typed raw pointers. Volatile load/store flags
are realized with an explicitly volatile-qualified dereference even when the
qualifier was introduced by a CogIR `ptr.qualify` value whose underlying C
expression had a less-qualified static type. `cast.checked` emits range/finite
guards before any host conversion that could otherwise be undefined; float-to-
integer conversion follows Coglet's truncate-toward-zero-then-fit rule, including
valid fractional values just outside an integer endpoint. `int.truncate` extracts
the destination-width low bits explicitly and reconstructs signed results without
implementation-defined narrowing, while `ptr.reinterpret` emits the typed/opaque
object-pointer representation conversion recorded by CogIR.
