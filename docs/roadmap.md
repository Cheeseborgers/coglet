# Coglet Roadmap

Coglet is focused on building a small, correct systems-language core with explicit semantics and a compiler architecture that remains understandable.

The project remains frontend-led, with a CogIR-only host-C bootstrap backend providing the executable feedback loop. The current host-C/CogIR contract is complete for the exercised language and interop surface. LLVM Stage 9 now provides a second optimized and source-debuggable native executable path: after verifier-checked lowering for the Stage 1-6 scalar/CFG, memory/aggregate, floating-point, function-value, C ABI, and x86-64 SysV/Win64 represented-object surface, Stage 7 emits a native object and links it through a separate host linker-driver boundary, Stage 8 adds target-aware LLVM `-O0` through `-O3` policy using LLVM's default optimization pipelines, and Stage 9 adds `-g` source debug metadata derived only from frozen CogIR provenance. Non-x86-64 aggregate classifiers remain part of future cross-target work.

## Current State

The lexer, parser, AST, compiler driver, semantic analyzer, and semantic test tooling support a substantial core language.

Implemented areas include:

- lexical scopes, symbol lookup, and shadowing
- explicit and inferred variables, parameters, functions, and constants
- canonical primitive numeric and boolean semantic types
- mutable and readonly raw object pointers with dedicated `null`, fixed arrays, nominal structs, nominal enums, and function types
- arithmetic, bitwise operations, shifts, comparisons, logic, calls, fields, indexes, casts, and aggregate initializers
- contextual fixed-array string literals, fixed-array `{0}` semantic-zero initialization, plus direct readonly C-string literal arguments at `#extern(c)` call boundaries
- assignment, arithmetic/bitwise/shift compound assignment, and increment/decrement as statement-only
  mutations
- lvalue/rvalue/no-value tracking with writable/readonly storage access
- `if`, `while`, `for`, `switch`, `break`, `continue`, and `return`, including single-statement control bodies and loop-scoped three-clause `for` initialization
- definite-assignment analysis for locals and parameters
- unified reachability for branches, switches, loops, returns, unreachable statements, and non-void
  fallthrough
- value-based Boolean and enum switch exhaustiveness
- nested functions without closure capture
- compile-time constant evaluation with exact signed-magnitude integers
- checked constant integer arithmetic, known zero-divisor diagnostics, and numeric representability checks
- constant array-index bounds checking
- IEEE-754 constant behavior for `f32` and `f64`, including infinity, NaN, and signed zero
- deterministic semantic-information verification and dumps
- stable semantic declaration identities and normalized native-C ABI metadata
- explicit frontend `TargetInfo` with host-default and synthetic cross-target semantic tests
- explicit contextual-conversion metadata for IR-ready expression lowering
- centralized, post-semantic constant-value metadata for IR lowering
- constant-aware loop reachability and explicit function-local control-flow isolation
- CogIR typed-CFG design with explicit ordered module initialization
- executable CogIR lowering for structured control flow, short-circuit logic, comparisons, loops, and switches
- CogIR data/address lowering for aggregates, pointers, fields/indexes, casts, volatile access, and strings

## Recently Completed

### Declaration Identity and ABI Normalization

The frontend-to-lowering boundary was hardened before CogIR implementation began.
Successful declarations now receive stable per-semantic-check IDs independent
of AST and symbol addresses, including parameters and aggregate members.

Native-C ABI intent is also normalized into semantic declaration metadata:

- function ABI, linkage, calling convention, variadics, and effective linker symbol;
- struct/union representation, incompleteness, packing, and alignment;
- represented-enum backing ABI type;
- recursive C-facing type spelling for ABI parameters and represented fields,
  preserving aliases such as `c_int`, `c_long`, and `c_char` after semantic type
  canonicalization.

The host-C backend consumes these normalized ABI facts rather than interpreting
`#extern(c)`/`#repr(c)` annotation fields directly. Semantic-info verification
checks the metadata against the successfully resolved program. This lets CogIR
lowering translate frozen semantic decisions rather than re-performing name, type,
or ABI interpretation.

### Explicit Frontend Target Description

Semantic C-scalar resolution now consumes an explicit `TargetInfo` rather than
querying `sizeof`, `CHAR_MIN`, or floating-format macros directly. The target
contract records pointer width, the C integer-family widths, plain-`char`
signedness, `_Bool` width, and C floating formats. The normal driver constructs
a host description for unchanged behavior, while a target-aware driver entry
point accepts synthetic/future targets. Tests compile the same source with two
different target descriptions and verify that `c_long`, `c_size`, and `c_char`
resolve differently. CLI target triples and non-host backend/toolchain selection
remain future work. The host-C backend explicitly rejects semantic state for a non-host target so an explicit frontend target cannot silently produce ABI-mismatched generated C.

### Explicit Contextual Conversion Decisions

`SemExprInfo` now preserves the conversion decision made by semantic analysis
instead of requiring a later backend to infer it from an AST expression and its
parent context. The intrinsic expression type remains unchanged, while a
separate contextual destination records materialization of adaptable numeric
values, `null`-to-pointer adaptation, safe raw-pointer qualifier addition, and
the narrow direct C-string call conversion.

`semantic_get_effective_expr_type()` exposes the type lowering should consume.
Inferred numeric variables/parameters, numeric operation operands, array
indexes and shift counts, assignments, returns, arguments, switch labels, and
typed constant/enum values now record their selected concrete use-site type.
The semantic-info verifier validates conversion invariants, and a dedicated
regression fixture exercises every current conversion kind.

Already-concrete C-variadic default promotions remain part of ABI lowering
rather than Coglet's implicit conversion system.

### Centralized Semantic Constant Values

Compile-time evaluation now has one exported `ConstValue` contract and one
post-check retrieval API, `semantic_get_constant_value()`. Successful constant
expressions are cached while semantic scopes are live, then exposed without
re-running lexical lookup or evaluator logic. Retrieval normalizes expression
values to the already-recorded effective contextual type.

Constant declarations and enum-member declarations, including implicit enum
values, carry their final compile-time value in `SemDeclInfo`; `Symbol` no longer
duplicates constant payloads. The semantic-info verifier checks these caches and
a dedicated regression exercises inferred and
typed constants, integer-to-float materialization, typed null, constant switch
labels, and implicit/explicit enum values. Fixed-array lengths remain literal
syntax today; general constant-expression array lengths are intentionally not
introduced by this milestone.

### Control-Flow Semantic Hardening

The pre-IR control-flow audit keeps `FlowState.reachable` as the single source of
truth while tightening the function-CFG contract. Infinite-loop recognition now
uses the centralized constant evaluator rather than matching only the literal
`true`, so named/local Boolean constants and other checked constant Boolean
expressions participate in non-void fallthrough analysis. Reachable `break`
paths still make those loops potentially continuing.

Regression coverage now explicitly locks down `return` outside a function and
function-local loop ownership: a nested function begins with no inherited loop,
so its `break`/`continue` statements cannot target an enclosing function's loop.

The audit also exposed a separate program-initialization policy. CogIR now
resolves that policy by representing program-scope runtime execution as an
explicit ordered module-initializer CFG. Source globals receive module-lifetime
storage before initialization; their explicit initializers and other top-level
runtime statements execute in source order. See `docs/ir.md`.

### Definite Assignment and Unified Reachability

Coglet now tracks whether each function-local variable and parameter is definitely initialized at every
reachable program point.

Completed behavior includes:

* local declarations without initializers remain uninitialized
* parameters and successfully initialized locals begin initialized
* direct whole-variable assignment initializes its target
* whole-struct and whole-array assignment initializes the complete variable
* field, element, pointer-index, and dereference writes do not initialize a complete base variable
* compound assignment and increment/decrement require prior initialization
* taking the address of an uninitialized local is rejected
* `if` branches are checked independently and merged by intersecting continuing paths
* omitted `else` branches preserve the unchanged incoming path
* switch cases begin from independent copies of the incoming state
* non-exhaustive switches include an implicit no-match path
* loops conservatively preserve the possibility of zero iterations
* `break` and `continue` flow targets the nearest loop
* `continue` and body-fallthrough paths reach a `for` post expression
* `break` and `return` paths do not reach a `for` post expression
* compile-time-true loops with no reachable `break` are non-continuing
* unreachable statements are diagnosed during block traversal
* a non-void function is accepted whenever normal control flow cannot reach the end of its body

Path-dependent initialization is stored in a separate `FlowState` rather than as mutable state on symbols.

Each tracked variable has an owner-qualified flow identity:

```text
(flow owner ID, variable ID)
```

This prevents nested functions from accidentally consulting an enclosing function's numerically identical
variable slot.

Nested functions do not currently support closure capture. They may access visible globals, constants,
types, and functions, but references to enclosing locals and parameters are rejected.

Switch exhaustiveness is based on successfully checked runtime values:

* `default` covers every value
* Boolean switches require both `true` and `false`
* enum switches require every distinct declared runtime value
* enum aliases with the same backing value require only one case
* invalid cases never contribute coverage

The older separate return-path and unreachable-analysis helpers have been removed. `FlowState.reachable` is now the single control-flow source of truth.


### Semantic Type and Numeric Hardening

The existing frontend semantics have been hardened without adding backend or
runtime execution work.

Completed areas include:

- canonical shared semantic instances for concrete built-in scalar types
- exhaustive structural type equality with declaration identity for structs and enums
- dedicated `null` semantics with no integer-zero pointer conversion
- direct diagnostics for `*null` and integer zero in pointer contexts
- explicit `null`-to-pointer casts
- equality restricted to supported value categories
- numeric-only ordered comparisons
- rejection of typed unsigned unary negation
- checked compile-time integer overflow and underflow
- shared known integer zero-divisor checks for binary and compound assignment
- IEEE-754 constant behavior for `f32` and `f64`
- correct unordered NaN comparisons and signed-zero handling
- expanded valid, invalid, constant-oracle, snapshot, and semantic-info coverage

The language-level runtime scalar contract is now selected:

- ordinary signed and unsigned integer arithmetic is checked;
- known failures are compile-time diagnostics;
- runtime-dependent failures trap;
- numeric cast is checked;
- semantics do not change between debug and release builds;
- shifts retain their existing fixed-width bit-pattern rules.

Explicit wrapping arithmetic and truncating integer conversion are implemented
as separate frontend operations rather than implicit behavior.

### Explicit Scalar Alternatives

The explicit scalar-alternatives milestone is complete.

Coglet now provides:

- checked ordinary signed and unsigned integer arithmetic in every build mode;
- checked numeric `cast`;
- central integer metadata and representability rules;
- runtime-dependent frontend conformance coverage;
- stable compiler builtin identities;
- `wrapping_add`, `wrapping_sub`, `wrapping_mul`, and `wrapping_neg`;
- `truncate(TargetIntegerType, expression)`;
- compile-time fixed-width evaluation for wrapping and truncating operations;
- semantic-expression verifier coverage.

Wrapping operations use fixed-width modulo arithmetic. Truncating conversion
retains the low destination-width bits and interprets them using target
signedness. These explicit operations do not change the checked semantics of
ordinary operators or `cast`.

### Mutable and Readonly Raw Object Pointers

Coglet's typed raw-pointer access milestone is complete.

Implemented forms:

```c
T*
readonly T*
```

Completed rules include:

- raw pointers remain nullable, non-owning, unchecked, and potentially dangling;
- `T*` grants mutable pointee access;
- `readonly T*` grants read access through that pointer;
- pointer bindings themselves remain independently assignable;
- mutable pointers adapt implicitly or explicitly to matching readonly pointers;
- readonly pointers cannot recover mutable access;
- qualification is shallow and is not recursively introduced through nested pointers;
- dereference and pointer indexing propagate pointer access;
- struct fields inherit the access of their object expression;
- address-of preserves writable or readonly storage access;
- matching mutable and readonly pointers may be compared;
- both forms adapt to and compare with `null`;
- semantic expression information distinguishes storage identity from write permission;
- semantic-info verification covers the access invariants.

This milestone does not add ownership, borrowing, lifetime checking, bounds
checking, pointer arithmetic, or deep immutability.

### Explicit Untyped Numeric Kinds

Numeric literals and adaptable constants now use:

```text
TYPE_UNTYPED_INT
TYPE_UNTYPED_FLOAT
```

The previous mixed state of a concrete kind plus an `is_untyped` flag has been removed.
Mutable inferred variables and parameters receive concrete default types (`s32`, `s64`, `u64`, or `f64`),
while inferred compile-time constants may remain adaptable.

### Hexadecimal Floating-Point Literals

Coglet accepts hexadecimal floating-point source literals using a mandatory binary `p`/`P` exponent, for example `0x1.8p1` and `0x1.921fb54442d18p+1`. They produce the same adaptable `TYPE_UNTYPED_FLOAT` semantic kind as decimal floating literals and materialize through the existing contextual `f32`/`f64` rules. The frontend freezes concrete IEEE payloads before CogIR, so neither backend needs hexadecimal-literal handling. `std.math.pi` and `std.math.tau` use this syntax and remain adaptable compile-time constants instead of being pre-cast to `f64`.

### Semantic-Information Verifier

The standalone verifier now:

- walks expression-containing AST locations in source order
- verifies one semantic entry per successful expression or mutation
- detects duplicate and orphan side-table entries
- checks value categories and symbol/type consistency
- distinguishes mutation nodes from void-returning calls
- rejects untyped numeric variable and parameter symbols
- prints deterministic semantic tables with `--dump-semantic-info`

### Closed Enums

Normal enums are now closed. The backing type determines representation and range,
while the valid enum values are exactly the declared member values.

Current cast rules:

- enum to integer: allowed
- compile-time integer to enum: allowed only for a declared member value
- runtime integer to enum: rejected until checked runtime conversion exists

This makes enum switch exhaustiveness sound. `#repr(c)` enums now provide an explicit C ABI representation through a required native C integer backing alias without making enums open.

### Bitwise and Shift Operators

Coglet now supports the integer bit-manipulation core:

- unary `~`
- binary `&`, `|`, and `^`
- `<<` and `>>`
- `&=`, `|=`, `^=`, `<<=`, and `>>=`
- integer-only operand checking
- contextual untyped integer adaptation
- exact concrete type matching for binary bitwise operations
- left-operand result typing for shifts
- statically known shift-count range diagnostics
- fixed-width truncating left shift
- zero-filling unsigned right shift
- arithmetic signed right shift
- compile-time evaluation using explicit width-limited bit patterns
- lexer, parser, semantic, diagnostic, constant-oracle, and semantic-info tests

Coglet intentionally gives bitwise operators higher precedence than equality
and ordered comparison, so `flags & mask == 0` means `(flags & mask) == 0`.

### Opaque Raw Pointers

Coglet's opaque raw-pointer milestone is complete.

Implemented forms:

```c
opaque*
readonly opaque*
opaque**
```

Completed rules include:

- `opaque*` is represented by the dedicated `TYPE_OPAQUE_POINTER` kind;
- there is no standalone opaque value type;
- opaque pointers are pointer-sized/address-like but cannot be dereferenced or indexed;
- mutable and readonly opaque variants follow the same shallow access model as typed raw pointers;
- mutable opaque pointers adapt implicitly or through `cast` to readonly opaque pointers;
- both access modes adapt to and compare with `null`;
- matching mutable and readonly opaque pointers may compare with each other;
- typed and opaque raw pointers do not implicitly convert or directly compare;
- `reinterpret(TargetPointerType, expression)` explicitly crosses between top-level typed and opaque raw pointers;
- `reinterpret` preserves address bits and cannot discard readonly access;
- `reinterpret` is not a general typed-pointer-to-typed-pointer cast;
- additional pointer layers compose normally, making `opaque**` a dereferenceable pointer to an `opaque*` slot;
- lexer, parser, semantic, diagnostic, and semantic-info coverage is included.

The future C ABI layer may map `opaque*` to `void*` without importing C's
implicit `void*` conversion semantics into Coglet.

## Candidate Next Design Work

The checked-scalar, explicit wrapping/truncation, typed raw-pointer, and opaque
raw-pointer milestones are complete. A narrow host-C backend now exists as the
first executable lowering path; broader runtime lowering remains incremental.

### 1. C Interoperability Design

The first external-declaration slice is now implemented:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;
```

This provides declaration-only top-level C-linkage functions, ordinary Coglet
call resolution, scalar/raw-pointer signature validation, and opaque-pointer
participation without C-style implicit `void*` conversions.

The executable host-C slice is implemented. The compiler can emit C, invoke the
CMake-configured native C compiler through the shared Linux/Windows toolchain
layer, resolve default C runtime/libc symbols, and honor `name="..."` overrides.
Portable C-identifier symbols no longer require GNU symbol-label syntax; arbitrary
non-identifier linker names retain that GNU/Clang-only extension. Integration
tests execute both default and overridden external symbols.

The first runtime/platform boundary is also implemented. `std.io`, `std.mem`, and the runtime-backed transcendental/rounding slice of `std.math` bind a reserved `coglet_rt_*` ABI supplied by split `stdlib/runtime/coglet_runtime_{io,math,mem}.c` components; host-C and LLVM select the same components from requirements derived when CogIR is frozen. `std.mem` now provides aligned typed allocation/resize/free, growable arenas, caller-buffer fixed arenas, scoped arena checkpoints, and a guard/poison/tracking debug allocator; `std.array.Array<T>` provides growable storage and `std.pool.Pool<T>` provides fixed-capacity generation-checked stable slots. The initial native-host matrix is Linux and Windows on x86-64 and AArch64. Future runtime-facing work such as virtual-memory allocators, time, filesystem/platform services, and threads should reuse this boundary rather than adding backend-specific calls.
Known temporary compromises are tracked in `docs/known_shortcomings.md`.

The native C scalar aliases are also implemented: `c_char`, `c_schar`,
`c_uchar`, `c_short`, `c_ushort`, `c_int`, `c_uint`, `c_long`, `c_ulong`,
`c_longlong`, `c_ulonglong`, and `c_size`, plus `c_bool`, `c_float`, and
`c_double`. They transparently resolve to canonical Coglet scalar types from the
selected frontend `TargetInfo`; the default driver supplies the native host
ABI. C floating aliases are enabled only when the selected target reports the
required IEEE binary32/binary64 formats. External
symbol-name overrides are implemented with `#extern(c, name="...")`. The host driver now
also accepts repeated `-L` search paths and `-l` libraries in joined or split
form and forwards them directly to `cc` for executable links. Direct string
literals can now bind to `readonly c_char*` parameters of `#extern(c)` calls and
are decoded/re-emitted by the host-C backend without enabling general
array-to-pointer decay. CLI cross-target selection and non-host backend/toolchain execution are still deferred.

The C aggregate representation slice now supports `#repr(c)` structs and unions
with scalar/raw-pointer fields, positive-length fixed arrays of supported field
types, nested represented aggregates by value, and `#repr(c)` enums with explicit
native C integer backing aliases. Inline aggregate dependencies, including
dependencies reached through array elements, are checked for cycles and emitted
in dependency order, so source declaration order does not constrain valid C
layouts. C unions currently act as ABI carrier values; direct member
construction/access remains deferred until an active-member policy is designed.

Incomplete named C structs are now represented with body-less declarations such
as `#repr(c) SDL_Window::struct;`. They are nominal, pointer-only foreign types:
the semantic layer rejects by-value storage/access and the host-C backend emits
only a forward declaration. This covers the common opaque-handle pattern used by
C libraries without conflating it with `void *`.

Native C callbacks/function pointers are now implemented with explicit
`cfn(...) -> T` types and top-level `#repr(c)` Coglet callback definitions. The
host-C backend emits real function-pointer typedefs and executable tests exercise
a C helper calling back into Coglet. C variadic declarations and variadic `cfn`
types are also implemented for the supported ABI value subset; the host C
compiler performs the standard default argument promotions. Native Coglet
variadics remain a separate future language-design problem.

Aggregate layout controls now include `#repr(c, packed)`, `#repr(c, align=N)`,
and their combination for complete structs/unions. Alignment values are positive
powers of two and denote minimum aggregate alignment; packed layout reduces
member alignment. The host-C bootstrap lowers these through guarded
GNU-compatible C attributes and executable tests verify size, alignment, field
offsets, and by-value calls. Explicit C calling conventions are also represented
with `call=cdecl`, `call=stdcall`, `call=sysv64`, and `call=win64` on extern
declarations, C callback definitions, and `cfn` types. Function-type identity
includes that convention, and an x86-64 executable regression crosses the
`win64`/`ms_abi` boundary in both directions.

The manual/native-host C interop surface is now intentionally paused at this stage. Remaining C interoperability work includes:

- CLI target triples/C-ABI presets and cross-compilation toolchain selection;
- richer linker/toolchain configuration beyond `-L` / `-l`;
- additional platform-specific ABI controls beyond the current `volatile`, layout, and calling-convention surface;
- mapping C null pointers to Coglet `null` at additional lowering boundaries.

### 2. Slices and Pointer-Length Views

The first slice milestone is implemented:

- `T[]` is a mutable non-owning slice and `readonly T[]` is a readonly slice;
- the frozen value representation is pointer + `u64` length, with no capacity or ownership;
- addressable fixed arrays adapt to matching slices;
- mutable slices weaken to readonly slices;
- string literals are inferred `readonly u8[]` byte views whose visible length excludes the compiler-owned trailing NUL;
- `.data`, `.len`, indexing, generic type inference through slice shapes, host-C lowering, and conditional LLVM coverage are wired;
- direct slice-by-value `#extern(c)` signatures remain rejected, so runtime wrappers cross C ABI boundaries as pointer + length.

Remaining slice work includes runtime bounds checking policy, reslicing/subviews, a target-sized length type instead of fixed `u64`, lifetime/escape safety, richer qualifier syntax for slices of pointer elements, and deciding whether an owning growable container carries capacity separately from the slice view.

A distinct first-class `string` type should still wait until ownership, encoding, mutation, and C-interoperability rules are clearer; `readonly u8[]` is currently the general byte-string view.

## Later Frontend Work

Source provenance and the compilation driver are multi-file-capable, and the
initial module/import namespace layer is now present. Files may declare a named
module, multiple files may contribute to one module namespace, and file-scoped
imports permit qualified function/global/constant/nominal-type access while
preserving one frozen CogIR compilation unit. Qualified globals retain ordinary
storage semantics and qualified constants retain compile-time value semantics.
Named-module declarations are private by default and explicit `export` marks the
public import surface; exported APIs are checked against private nominal-type leaks.
The command-line driver now resolves missing hierarchical imports from a canonical
source path by mapping dots to path separators (`std.math` -> `std/math.cog`) beside
the importer or under repeated `-I` roots, including transitive imports and import
cycles. The compiler also has a configured install-prefix-aware standard-library
module root consulted last for unresolved `std`/`std.*` imports, with CLI override
and query support for packaging/development. Semantic lookup uses the longest visible
module prefix so hierarchical module qualification composes with ordinary field
access using one dot operator. Discovery is source ingestion only: explicit roots
retain command-line order, discovered files follow in first-discovery order, and
imports do not become runtime dependency edges.

- grow the initial pure-Coglet `std.math` module and add package manifests / automatic multi-file package membership
- stable declaration identity for future separate compilation
- stable declaration identity across files
- diagnostic notes/secondary spans and richer recovery
- a standard library design
- closure and capture semantics, only if nested runtime functions require them
- broader generics such as generic enums/aliases or user-defined constraints, if justified by real use cases

## Execution Work

The host-C transpilation backend remains the bootstrap execution path, while the
compiler-owned CogIR path now lowers checked scalar execution, structured CFGs,
module initialization, aggregate values, general places/storage, pointer
addressing, volatile access, casts, and string/character values. The large
semantic-valid integration fixture lowers and verifies through CogIR after its
uninitialized-array cases are made semantically valid.

Explicit wrapping builtins now lower to dedicated CogIR wrapping operations, and
all 100 semantic-valid fixture programs lower and verify through `dump_ir`. Native-C
variadic tails are legalized with explicit `c.vararg.promote` operations, and the
verifier rejects unpromoted or otherwise ABI-illegal tail values.

The host-C boundary move is now complete without leaking the host process ABI
into Coglet entry semantics. A source-top-level executable entry is
`main::() -> s32`; semantic analysis validates that spelling before C aliases are
canonicalized, and CogIR carries only the resolved entry identity plus its
backend-neutral runtime type. `backend_c.h` accepts only `const CogIrModule *`;
the driver lowers, verifies and freezes CogIR, destroys the frontend, then
emits/builds C. The host-C emitter supplies the C `int main(void)` adapter. The
LLVM backend maps the same CogIR entry to the native process ABI, and future
native backends must preserve that backend-neutral entry contract. The host-C
executable and C-interop suite therefore runs through frontend -> CogIR -> C.

The host-C emitter has expanded beyond the old backend's deliberately narrow
execution subset. Dedicated wrapping integer operations execute with explicit
modulo-width bit-pattern lowering, including signed edge cases without host signed
overflow. Checked integer add/subtract/multiply, division/remainder, and signed
negation execute with explicit precondition guards and abort on the CogIR
arithmetic-trap path. Reachable multi-block CFGs now emit as labels/gotos with
parallel block-parameter edge transfer, conditional/unconditional branches,
switches, traps, and unreachable terminators. Integer predicates, scalar globals,
non-trapping bitwise operations, checked-count shifts, floating arithmetic/
comparisons/negation, and pointer equality complete the exercised scalar
module-initializer/control-flow path. The next compatibility slice is also complete:
represented field/array-field places, typed-pointer indexing, volatile scalar
loads/stores, checked numeric casts, truncating integer conversion, and typed/
opaque pointer reinterpretation now execute from CogIR. Checked casts guard
range/non-finite failures before host conversions, and truncation reconstructs
signed destination values from explicit low-bit patterns. First-class native-C
function values now retain exact C ABI spelling across parameters, locals, CFG
spills, and callback-valued C calls; the host-C emitter executes fixed and
variadic indirect `cfn` calls from that metadata. Aggregate values and globals now
execute from CogIR too: assignable wrapper structs represent first-class Coglet
array values, native C arrays remain the storage/layout representation, aggregate
construction/extraction and by-value calls use the same boundary, represented
aggregate interop preserves exact C field layout, and strings share the general
array-storage path. The final host-C parity/cleanup audit is complete: every
current CogIR operation has an explicit emitter path, backend entry points accept
only frozen CogIR, frontend lifetime ends before emission, and source executable
entry selection is carried by `module.entry_function` rather than a debug-name
heuristic. This preserves the source-top-level `main` rule after nested functions
are flattened, while the verifier enforces the native Coglet `() -> s32` entry
signature. The CogIR-only host-C bootstrap path is therefore complete for the
current executable/interop contract. LLVM Stage 6 established the same frozen,
verified-module boundary and additionally lowered target C object storage and
represented aggregate ABIs. Exact C `_Bool`
storage is distinct from logical `bool` values, represented C structs/unions and
arrays honor packed/explicit alignment, and x86-64 SysV/Win64 aggregate calls use
an explicit classifier that can split values into registers or select indirect
`byval`/hidden-`sret` passing. Declarations, calls, and Coglet-defined C callbacks
all consume the same backend ABI plan. LLVM-specific target layout and callable
signatures remain backend-owned derivations of frozen CogIR; no frontend object is
consulted after lowering. LLVM Stage 7 reuses that same verified module and
backend-owned target machine to emit native objects and links them through a
separate host toolchain boundary. Executable-link objects use LLVM PIC relocation
so hosts that default to PIE link correctly even at `-O0`; this remains backend
policy and does not enter CogIR. Textual LLVM IR emission remains available.
LLVM Stage 8 adds user-visible
`-O0`/`-O1`/`-O2`/`-O3` policy without changing CogIR: O1-O3 run LLVM's default
new-pass-manager pipelines with the backend target machine, optimized modules are
verified again before output, and target code-generation intensity follows the
same level. Stage 9 adds `-g` without changing CogIR into an LLVM-metadata
container: source files/spans and explicit source-local/parameter slot provenance
are frozen in CogIR, while DI construction stays backend-owned. Compiler temporary
slots and the synthetic process-entry adapter are not presented as Coglet source
variables/code. The LLVM tooling path can now also emit native target assembly
directly from the same verified/optimized `TargetMachine` pipeline; assembly uses
PIC relocation to match executable code generation and may be retained alongside
a normal LLVM executable build. General-purpose optimization remains LLVM-owned;
aggregate classification for additional targets waits for explicit cross-target
support.

Longer-term additions remain possible, including a custom native backend or an
interpreter for tooling and compile-time execution. The host-C path continues to
provide executable feedback without defining Coglet semantics by C behavior.

## Self-Hosting Direction

Self-hosting remains a long-term objective rather than the next milestone. Basic runtime I/O and heap allocation now exist, but the language still needs file I/O, stronger ownership/resource cleanup, diagnostics suitable for larger programs, and a stable package/module/standard-library boundary before self-hosting becomes practical.

## Generic monomorphization milestones

The first restricted generic-function facility is implemented as frontend-only,
compile-time monomorphization. Type parameters can be inferred from ordinary
arguments or supplied explicitly with `::<...>`; each concrete body is rechecked
under ordinary semantic rules before CogIR lowering. A restricted contract layer
permits one closed builtin constraint on each type parameter (`integer`,
`signed_integer`, `unsigned_integer`, `floating`, `numeric`, or `ordered`).
Constraint satisfaction is checked before specialization and does not replace
ordinary specialization-time semantic checking.

The first generic nominal-type milestone is also implemented for ordinary
top-level Coglet structs. `Pair::<T, U> struct { ... }` templates specialize to
ordinary concrete nominal layouts before CogIR. Concrete applications require
explicit type arguments, reuse a cache keyed by template declaration identity plus
structural concrete arguments, support finite pointer recursion, reject recursive
by-value layouts, obey module/export visibility, and participate in generic-function
inference through parameter shapes. CogIR and both backends remain generic-unaware.

Struct methods and associated functions are now implemented for ordinary complete
Coglet structs, including generic struct specializations. They are declared in the
struct body, use `Self` for the concrete owner, support value and pointer receivers,
and desugar to ordinary function calls before CogIR. The initial facility avoids a
separate implementation-block language and does not add dispatch machinery. Method
receiver expressions that have already been checked are reused during call rewriting,
so fluent value-call chains such as `v.add(other).scale(0.5)` remain valid.

`std.math` now uses these facilities for ordinary-source `Vec2<T>`, `Vec3<T>`, and
`Vec4<T>` numeric vectors. The first slice includes constructors, component arithmetic,
dot products, `Vec3.cross`, squared length/distance, component min/max/clamp, and
floating-specialization `length`, `distance`, and `normalized` methods.

A restricted struct-operator facility is now implemented on top of methods rather
than a separate trait/dispatch system. Structs explicitly map binary `+`, `-`, `*`,
`/` and unary `-` to by-value `Self` methods; the corresponding compound assignments
reuse those mappings while evaluating the target place once. `std.math` vectors use
this for vector addition/subtraction, scalar multiply/divide, negation, and common
gameplay forms such as `position += velocity * dt`. Component-wise multiply remains
the explicit `.mul(...)` method.

A deliberately strict exact-overload facility now provides type-directed runtime math
without stdlib compiler magic: ordinary non-generic functions may share a name when
their concrete parameter lists differ, and calls select only an exact match after
normal untyped-numeric defaulting. Generic-struct method bodies are checked lazily on
first concrete use, so integer vector specializations remain valid while a floating-
only body is diagnosed only if called for an integer specialization.

The first transform-math layer is also implemented entirely as ordinary standard-
library source: `Quat<T: floating>`, `Mat3<T: floating>`, and `Mat4<T: floating>`
provide quaternion composition/interpolation/vector rotation, matrix rotation/scaling/
translation/TRS construction, matrix multiplication, and point/vector transforms.
Matrices use a documented column-vector convention with `A * B` applying `B` first.
Projection helpers remain deferred until handedness and clip-depth conventions can be
made explicit rather than hidden behind an ambiguous default.

Remaining generic/type-system design work includes generic enums/aliases, generic
represented-C aggregates if justified, generic methods, user-defined
traits/constraints, broader operator categories/equality, specialization, dynamic
dispatch, type erasure, and separate generic compilation. Vector/matrix and container
APIs should continue to drive any expansion of the deliberately narrow operator set.
