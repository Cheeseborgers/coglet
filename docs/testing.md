# Testing

Coglet uses CTest to run lexer, parser, semantic, constant-evaluation, semantic-information, host-C backend integration tests, and LLVM backend tests when LLVM 17+ development files are available.

Configure and build the Debug tree before running tests:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug
```

## Complete Test Suite

Run every registered test with:

```bash
ctest \
    --test-dir cmake-build-debug \
    --output-on-failure
```

The exact number of tests changes as coverage is added. Do not treat a previously recorded total as
authoritative.

Target/layout coverage includes synthetic 32-bit and 64-bit pointer targets for
`usize`/`isize`, parser rejection of the obsolete `size_of::<T>()` /
`align_of::<T>()` spelling, and semantic/CogIR/backend coverage for the current
`size_of(T)` / `align_of(T)` forms.


Static-assert coverage includes parser snapshots, top-level and block assertions, forward and local constants, enum/Boolean/arithmetic conditions, optional diagnostic messages, non-Boolean and runtime-value rejection, generic-specialization checking, and target-layout assertions for scalars, arrays, slices, and aggregates.

Result-use coverage checks the default must-use rule for value-returning calls,
explicit `discard`, `#discardable` ordinary/generic/method/extern declarations,
`discard` in loop-post and deferred statement positions, rejection of `discard` in
value contexts, the resource-result prohibition, and preservation of ordinary bare
arithmetic/comparison statement expressions. Backend fixtures also keep
intentionally discarded call results warning-free without weakening CogIR
result-use verification.

## Test Categories

The test suite includes:

* lexer token snapshots;
* parser AST snapshots;
* invalid parser diagnostic snapshots with non-zero exit verification;
* valid semantic programs;
* invalid semantic diagnostic snapshots;
* compile-time constant-value oracles;
* semantic-information snapshots and verification;
* host-C backend compile/link/run integration tests and backend rejection tests.

CTest labels allow related tests to be selected without depending on test-number ranges.
The stable top-level labels are `lexer`, `parser`, `semantic`, `ir`, `driver`,
`backend`, and `stdlib`; backend-specific selection uses `backend.c` or
`backend.llvm`. Feature labels such as `module`, `resource`, `interop`, `debug`, or
`optimization` refine those families. Historical implementation-stage labels are
not part of the test interface.

To list all registered tests:

```bash
ctest \
    --test-dir cmake-build-debug \
    --show-only
```

To list tests and their labels:

```bash
ctest \
    --test-dir cmake-build-debug \
    --show-only=json-v1
```

## Driver version test

`driver_version` runs `coglet --version` without an input file and checks the
reported version against CMake's configured project version. The test is
registered in both LLVM-enabled and LLVM-disabled configurations so compiler
identity does not accidentally depend on an optional backend.

Run it with:

```bash
ctest --test-dir cmake-build-debug -L version --output-on-failure
```

## Build-tree test artifacts

CTest-generated runtime products are kept under:

```text
<build-dir>/test-artifacts/
```

This includes backend test executables, emitted `.ll`/`.s` files, trap outputs,
and the convenience copy of `tests/test_assets`. Test/developer helper binaries (`check_*`, `dump_*`) live under
`test-artifacts/tools`, and the test-only linker support archive lives under
`test-artifacts/lib`. The compiler and normal compiler/backend libraries remain
in their ordinary build locations. CTest still owns its standard
`<build-dir>/Testing/` metadata directory.

This separation keeps repeated full-suite runs from filling the build root with
hundreds of generated programs and intermediate files.

## Standard-library root tests

The `stdlib` label covers the compiler-configured `std.*` module fallback and its
packaging/development controls. The tests verify exact `--print-stdlib-root`
output, split/joined `--stdlib-root` overrides, importer-directory and user `-I`
precedence over the stdlib fallback, transitive `std.*` discovery, explicit-module
suppression, rejection of malformed root options, and the rule that non-`std`
module names never consult the compiler-owned stdlib root. LLVM-enabled coverage
also runs an optimized/debuggable native build and checks debug provenance from
a stdlib-root-discovered source.

Run the slice with:

```bash
ctest --test-dir cmake-build-debug -L stdlib --output-on-failure
```

## Shipped standard-library source tests

The actual shipped standard-library source lives under `stdlib/`. `std.math`, `std.io`, `std.array`, and the memory-pattern modules (`std.mem`/`std.pool`) have ordinary consumer programs under `tests/stdlib/`. These tests use
the `stdlib.source` label so they are distinct from the lower-level stdlib-root/
discovery-policy fixtures. The `std.io` integration test links the runtime through
the same executable path users invoke and compares its complete stdout, covering
readonly byte-slice string output, Boolean output, every fixed-width integer printer, `f32`, `f64`, newline, and flush. The main `std.math` integration test exercises both precisions of
the runtime-backed square-root, trigonometric, inverse-trigonometric, rounding,
and floating-remainder functions. A dedicated vector consumer instantiates
`Vec2`, `Vec3`, and `Vec4` with signed, unsigned, and floating scalar types and
checks fluent method chaining, component arithmetic, dot/cross products, squared
length/distance, and component min/max/clamp. LLVM-enabled configurations run the
same vector consumer at `-O0` and `-O3`. Transcendental comparisons use tolerances;
rounding/remainder cases use exact expected values. The `std.array` consumer covers target `size_of`/`align_of`, aligned typed allocation, geometric growth, mutable/readonly slice views, pop/reserve/clear, and explicit deinitialization. A dedicated memory-pattern consumer covers nested scratch checkpoint/rewind, caller-owned `FixedArena` storage/reset, and generation-checked fixed-capacity `Pool<T>` handles. A separate debug-allocator consumer checks live/total accounting, allocate/resize/free preservation, use through `Array<T>`, allocation poisoning, and front/back guard validation. Separate failure-path tests verify that runtime-backed modules diagnose a selected stdlib root that omits the runtime component required by their frozen CogIR declarations.

Run them with:

```bash
ctest --test-dir cmake-build-debug -L stdlib.source --output-on-failure
```

or through the build target:

```bash
cmake --build cmake-build-debug --target check_stdlib
```

LLVM-enabled configurations compile and execute each consumer through host-C,
LLVM `-O0`, and LLVM `-O3`. LLVM-disabled configurations retain the host-C tests.
Generated executables are placed under `test-artifacts/`.

See `docs/stdlib.md` for the public API and manual source-tree commands.

## Fixed-array zero-initializer tests

The `{0}` array-initializer coverage checks parser spelling, contextual semantic
typing, rejection without an expected array type or against scalar storage,
semantic-info completeness, and executable host-C/LLVM lowering through CogIR's
typed `zeroinit` constant. The backend fixture exercises local/global
declarations, whole-array assignment, call arguments, returns, and array-valued
struct fields.

Run the focused slice with:

```bash
ctest --test-dir cmake-build-debug -R zero_initializer --output-on-failure
```

## Generic Function and Struct Tests

Generic coverage spans parser snapshots, valid semantic/semantic-info fixtures,
focused invalid diagnostics, CogIR golden dumps, host-C execution, module/export
visibility, and conditional LLVM-native execution. The generic-struct fixture
checks repeated specialization reuse, nested applications such as
`Box::<Pair::<s32, f32>>`, builtin constraints, pointer-recursive layouts,
generic-function inference through `Pair::<T, U>`, and the rule that no generic
template reaches CogIR. Invalid fixtures cover missing/wrong type arguments,
constraints, generic application to an ordinary struct, private export leakage,
by-value recursive layouts, nested generic declarations, and non-terminating
changing specialization chains.

Run the focused generic slice with:

```bash
ctest --test-dir cmake-build-debug -L generic --output-on-failure
```

## CogIR Lowering Tests

CogIR golden dumps cover executable entry identity, structured-CFG, data/address,
explicit wrapping lowering, and native-C variadic default promotions. The entry
golden demonstrates that a source `main::() -> s32` lowers to the distinguished
backend-neutral `module.entry_function` without source C-scalar spelling
metadata. The variadic golden covers both direct external calls and
indirect `cfn(..., ...)` calls, including `bool`, narrow signed/unsigned integers,
`#repr(c)` enums, and `f32`. A verifier regression separately rejects an
unpromoted Boolean variadic tail. A separate integration test recursively runs `dump_ir` over
every program under `tests/test_assets/semantic/valid/`, so adding a new
semantic-valid fixture also extends the frontend -> CogIR compatibility gate.

Run the CogIR suite with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L ir \
    --output-on-failure
```

## Host-C Backend Tests

Backend integration fixtures live under:

```text
tests/test_assets/backend/
```

Host-C test registration uses the helpers in `cmake/HostCBackendTests.cmake`,
which centralize compile/run, explicit-library, trap, and expected-failure CTest
plumbing. The helpers are intentionally host-C-specific until another backend
establishes which parts of that interface are actually common.

The executable tests run the Coglet compiler with `-o`, invoke the generated
program, and verify its process exit status. These tests now exercise the full
frontend -> CogIR -> host-C boundary: the compiler freezes/verifies CogIR and
destroys the frontend before backend emission, so every passing backend fixture
also guards against accidental AST/semantic lifetime dependencies. Entry-point
coverage executes `main::() -> s32 { return 42; }`, rejects the obsolete
`main::() -> c_int` contract, and verifies that a nested function named `main` is
not accepted as the process entry after CogIR flattens nested functions into the
module function table. The C-interoperability cases cover
default external symbols, `#extern(c, name="...")` overrides, explicit C calling
conventions (including an x86-64 `win64` callback round trip), and explicit
`-L`/`-l` resolution against a test-only static library using both split and
joined flag forms. A separate driver-negative test verifies that linker flags
are rejected without `-o`. Checked-arithmetic backend tests now execute successful
signed/unsigned `+ - * / %` and signed negation through CogIR, with separate trap
regressions for overflow, division by zero, and signed-minimum/`-1`. The trap
harness verifies that the generated executable does not reach its normal fallback
exit. Backend-negative coverage still verifies that an unsupported explicit
calling convention fails rather than being ignored. The incomplete-aggregate
interop case also exercises runtime
pointer-qualification lowering across a C call result before C emission. The
wrapping-arithmetic executable regression checks runtime signed/unsigned narrow
wraparound and the signed 64-bit minimum edge through dedicated CogIR wrapping
operations rather than constant folding. The structured-CFG backend regression
executes ordered scalar global initialization plus top-level `if`, short-circuit
block-parameter transfer, function `if/else`, a loop backedge/comparison, and a
`switch` through the CogIR-only backend. It also guards reachability-based omission
of dead synthetic CFG blocks from generated C. The scalar-operations backend
regression executes integer bitwise operators, `f32`/`f64` arithmetic and
comparisons, floating negation, and pointer equality/inequality through runtime
function parameters so those paths cannot disappear through constant folding.
The shift backend regression covers fixed-width left shift, zero-filling unsigned
right shift, arithmetic signed right shift (including count zero), and separate
runtime traps for negative and width-sized counts. The data/address/cast backend
regression executes represented-aggregate field access, nested array-field
indexing, typed-pointer indexing, volatile access through an explicitly qualified
pointer value, typed/opaque pointer reinterpretation, checked integer/float casts,
and signed/unsigned integer truncation. Separate trap regressions cover a negative
signed-to-unsigned checked cast, an out-of-range float-to-integer cast, and a finite
`f64` value outside the `f32` range. The success case includes fractional values at
integer boundaries to verify that float-to-integer checking applies truncation
toward zero before representability testing.
The indirect-call backend regression covers fixed callback parameters, explicit
and inferred `cfn` locals, a callback callee preserved across short-circuit CFG
spilling, indirect C variadic calls after CogIR default promotions, and a callback
value returned by an external C function. This keeps exact callback ABI spelling
in the tested IR/backend contract rather than relying on representation-compatible
host-C function pointer types.
The aggregate-values backend regression covers array arguments/returns and value
copies, ordinary structs containing arrays, ordered array/struct global
initialization, represented C structs with nested array fields, whole-array
extraction/reconstruction, and by-value represented aggregate interop. It also
keeps array value-vs-storage representation observable: Coglet array rvalues are
assignable wrapper values while addressable and represented fields remain native C
array storage. Existing string regressions exercise the same generalized array
storage path, and volatile array copies use element-wise accesses rather than
`memcpy`.

Run the backend suite with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L backend \
    --output-on-failure
```

Backend-generation failures from the `coglet` executable currently use process
exit status `3`; parser/driver and semantic statuses remain unchanged.


## LLVM Backend Tests

LLVM support is optional at configure time. `COGLET_LLVM=AUTO` (the default)
enables it when CMake can find `LLVMConfig.cmake`; use `COGLET_LLVM=ON` to make
missing LLVM 17+ development files a configuration error or `COGLET_LLVM=OFF` to
disable the backend deliberately.

LLVM tests are grouped by stable capability rather than implementation order. IR
compatibility cases emit through the compiler, so successful cases have already
passed the LLVM verifier; when `clang` is available, those harnesses can also compile
the textual IR and check process status. Native-backend cases use `--backend llvm`
directly so the compiler verifies LLVM IR, emits a native object with its
`TargetMachine`, links it, and executes the result without routing through textual
`.ll` output.

The core LLVM slice covers scalar/structured CFG, ordered module initialization,
checked and wrapping integer semantics, traps, shifts, enum/integer conversions,
floating arithmetic/comparisons/conversions, pointers, ordinary aggregates, switch,
and indirect native Coglet function calls. Memory tests cover target triple/data
layout, fields/arrays/pointer indexing, aggregate values/copies/arguments/returns,
globals, and explicit rejection of unsupported volatile whole-aggregate access.

Interop tests link against an independently compiled C support library. They cover
exact C scalar aliases, small-integer ABI attributes, callbacks/returned function
pointers, variadics/default promotions, calling conventions, incomplete and volatile
pointers, external symbol overrides, represented structs/unions/enums/arrays,
packed/explicit alignment, represented globals, addressable C `_Bool`, and x86-64
SysV/Win64 aggregate passing. Negative coverage keeps ordinary Coglet `bool*`
distinct from `c_bool*`.

Native code-generation coverage includes PIC executable linking, `-O0` through
`-O3`, optimized textual IR, checked-overflow traps after optimization, and driver
rejection of optimization requests that the selected output path cannot honor. Debug
coverage checks compile units, locations, source functions/globals/parameters/locals,
aggregate/enum/pointer types, exact represented-C metadata, optimized debug IR, and
native `-g` execution. Compiler-generated slots and the synthetic process-entry
adapter are checked not to masquerade as source entities. Assembly tests cover PIC
round-tripping, optimized debug assembly, and combined assembly + executable output.
LLVM-disabled configurations retain negative driver coverage for LLVM-only flags.

Multi-file compilation coverage exercises a shared global namespace through both
host-C and LLVM with the input order reversed between backend tests, cross-file
nominal type/function resolution, cross-file duplicate diagnostics, parser-error
source provenance, and LLVM `-g` metadata containing both physical source files.
The multi-file tests use `-L multi-file` labels where applicable.

Module/import coverage builds named modules from multiple physical files and
checks qualified functions, globals, constants, structs, and enum members through
host-C and LLVM. Qualified-data coverage includes writable/addressable globals,
array/struct subobjects, qualified constants in compile-time contexts, input-order
independence for module data used by function bodies, constant-cycle diagnostics, and
same-name global/constant isolation between modules. It also covers file-scoped
imports, unknown and duplicate namespace diagnostics, permitted compile-time import
cycles, root-only executable entry selection when a named module also declares
`main`, private-by-default imported declarations, explicit exported functions/data/
nominal types, same-module private access, root-namespace export rejection, and
rejection of exported interfaces that expose private nominal types. Discovery
coverage verifies sibling `name.cog` loading, split/joined `-I`, transitive imports,
import cycles, importer-directory and search-root precedence, explicit-module
suppression, parser provenance in discovered files, deterministic module-init order,
and LLVM debug provenance for discovered sources. Hierarchical-module coverage adds
dotted module/import parsing, `std.math` -> `std/math.cog` discovery, transitive dotted
imports, qualified types/constructors/enums/data/functions, longest-visible-prefix
resolution against ordinary runtime fields, missing-import diagnostics, and LLVM
optimized/debuggable native output. Use `-L module`, `-L import`, `-L discovery`, `-L dotted`, `-L package`, `-L stdlib`,
`-L visibility`, `-L export`, `-L private`, `-L global`, or `-L constant` for focused
namespace/data/visibility/discovery slices.

Run only these tests with:

```bash
ctest --test-dir cmake-build-debug -L backend.llvm --output-on-failure
```

Run the assembly-emission slice with:

```bash
ctest --test-dir cmake-build-debug -L assembly --output-on-failure
```

## Parser-Invalid Tests

Invalid parser inputs live under:

```text
tests/test_assets/parser/invalid/
```

Each `.cog` fixture has a matching `.expected` snapshot. The parser-invalid
harness requires `dump_ast` to exit with status `1`, requires stdout to remain
empty, and compares normalized stderr diagnostics. Checkout-specific filename
and line/column prefixes are removed before comparison so snapshots remain
portable.

Run them with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L parser.invalid \
    --output-on-failure
```

## Semantic Tests

Run the complete semantic suite with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L semantic \
    --output-on-failure
```

Semantic tests are divided into valid and invalid programs.

### Valid semantic tests

Valid programs are stored under:

```text
tests/test_assets/semantic/valid/
```

A valid test succeeds only when parsing and semantic analysis complete without diagnostics.

Run only valid semantic tests with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L semantic.valid \
    --output-on-failure
```

### Invalid semantic tests

Invalid programs are stored under:

```text
tests/test_assets/semantic/invalid/
```

Each invalid source file has a corresponding `.expected` file containing the exact diagnostics and
final error-count summary.

For example:

```text
tests/test_assets/semantic/invalid/types/example.cog
tests/test_assets/semantic/invalid/types/example.expected
```

Run only invalid semantic tests with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L semantic.invalid \
    --output-on-failure
```

Diagnostic snapshots intentionally verify:

* diagnostic wording;
* source line numbers;
* diagnostic ordering;
* the final semantic error count;
* the expected process exit status.

A test should not be updated merely to make a failure disappear. First verify that the new output
represents the intended language behavior.

## Generating Expected Semantic Diagnostics

Expected files for invalid semantic tests can be generated with:

```bash
./get_expected.sh
```

The generator normalizes diagnostic source paths to `<source>` so snapshots remain checkout-independent.

Review every generated change before accepting it. A bulk expected-file update can conceal a diagnostic
regression or an unintended cascade.

To inspect one invalid program directly:

```bash
./cmake-build-debug/test-artifacts/tools/check_semantics \
    tests/test_assets/semantic/invalid/types/shadowed_struct_is_distinct.cog

echo $?
```

Semantic failures use process exit status `1`.

Parser and driver failures use process exit status `2`.

## Semantic-Information Tests

Semantic-information tests verify the expression side table produced during semantic analysis.

Run them with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L semantic.info \
    --output-on-failure
```

These tests cover facts including:

* resolved expression types;
* associated symbols;
* lvalue, rvalue, and no-value categories;
* writable, readonly, and no-storage access classifications;
* valid category/access combinations;
* dereference, index, field, and address-of access propagation;
* completeness and uniqueness of side-table entries;
* absence of orphan entries;
* canonical built-in scalar types;
* declaration and symbol associations.

An expression may retain valid resolved facts even when a later flow-sensitive rule rejects its use.
For example, an identifier can retain its type, symbol, and lvalue category while definite-assignment
analysis reports that the variable may be uninitialized.

## Compile-Time Constant Tests

Constant-oracle tests verify the evaluated values and semantic types of compile-time constants.

They cover behavior including:

* exact integer values;
* decimal and hexadecimal floating-point literal materialization, including exact `f32`/`f64` CogIR payloads for representative hexadecimal constants;
* overflow and representability;
* integer division and remainder diagnostics;
* checked casts;
* truncating integer conversion;
* wrapping integer builtins;
* Boolean operations;
* enum values;
* bitwise operations;
* shifts;
* `f32` rounding;
* IEEE-754 infinity, NaN, and signed zero.

Run the relevant registered tests by label or name pattern. For example:

```bash
ctest \
    --test-dir cmake-build-debug \
    -R "constant|const" \
    --output-on-failure
```

## Definite-Assignment Coverage

Definite-assignment tests use the label:

```text
semantic.definite_assignment
```

Run them with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -L semantic.definite_assignment \
    --output-on-failure
```

Coverage includes:

* initialized and uninitialized local reads;
* parameters;
* direct whole-variable assignment;
* compound assignment;
* increment and decrement;
* whole-array and whole-struct assignment;
* field and indexed writes;
* address-of and dereference;
* `if` branch merging;
* exhaustive and non-exhaustive switches;
* conservative loops;
* `break` and `continue`;
* unreachable statements;
* non-void function fallthrough;
* non-terminating compile-time-true loops;
* nested loops;
* nested-function flow ownership;
* rejection of unsupported nested-function captures;
* semantic-information recording for rejected reads.

Tests for another semantic rule may need to initialize arrays or structs explicitly so that
definite-assignment diagnostics do not mask the behavior the test is intended to exercise.

## Raw-Pointer Coverage

Pointer semantic tests cover:

- mutable and readonly pointer syntax;
- mutable-to-readonly initialization, assignment, arguments, and returns;
- explicit mutable-to-readonly casts;
- rejection of readonly-to-mutable conversion;
- rejection of recursive nested-pointer qualification;
- valid reads through readonly pointers;
- rejection of assignment, compound assignment, increment, and decrement through readonly pointers;
- access propagation through dereference, pointer indexing, array indexing, and fields;
- preservation of readonly access through `&*pointer`;
- equality between matching mutable and readonly pointers;
- null casts and comparisons for both pointer access modes;
- semantic-info verification of storage access facts.

## Focused Test Runs

CTest regular expressions are useful while implementing a small compiler layer.

For example:

```bash
ctest \
    --test-dir cmake-build-debug \
    -R "nested_function_(capture|non_capture)" \
    --output-on-failure
```

Switch-specific tests can be selected with:

```bash
ctest \
    --test-dir cmake-build-debug \
    -R "switch" \
    --output-on-failure
```

Method and associated-function work can be selected by name:

```bash
ctest \
    --test-dir cmake-build-debug \
    -R "methods|method_" \
    --output-on-failure
```

The method coverage includes parser snapshots, valid and invalid semantic cases,
semantic-info verification, CogIR desugaring snapshots, host-C execution, module
visibility, and conditional LLVM execution. Important negative fixtures cover bad
receiver types, non-first `self`, pointer receivers on temporaries, associated
functions called through values, instance methods called through types, field/name
conflicts, and exported signatures that leak private types.

A focused run does not replace the complete semantic or full regression suite.

## Sanitizer Verification

A separate build may be configured with AddressSanitizer and UndefinedBehaviorSanitizer.

Run the semantic suite against that build after changes to:

* arenas and ownership;
* semantic types;
* flow-state arrays;
* scopes;
* constant evaluation;
* AST or side-table storage.

Coglet intentionally uses arena lifetime management. Leak reporting may need to be disabled when
the purpose of the run is to detect invalid accesses and undefined behavior rather than arena-wide
lifetime retention.

## Test Design Requirements

Every semantic rule should have focused positive and negative coverage where both are meaningful.

Tests should verify the intended rule directly rather than passing or failing because an earlier
unrelated diagnostic masks it.

When adding or changing a semantic feature, consider coverage for:

* valid behavior;
* invalid behavior;
* boundary values;
* nested scopes;
* shadowing;
* malformed expressions;
* diagnostic source lines;
* diagnostic ordering;
* semantic-information facts;
* interaction with existing control flow.

Do not claim a test passes until it has actually been run.

## Parser Failure Fixtures

Expected parser failures live under `tests/test_assets/parser/invalid/` and have
paired `.expected` diagnostic snapshots. This includes the diagnostic-list growth
stress case and invalid enum default-type syntax; they are registered through the
non-zero parser harness rather than being silently skipped by the successful AST
snapshot harness.

- Grouped typed mutable declarations are covered across parser, semantic, host-C, and LLVM paths.


### Conditional expressions

The semantic/IR fixture suite covers braced `if` expressions, nested `else if` chains, incompatible branch types, missing `else` diagnostics, CFG lowering, and resource ownership transfer through the selected branch.
