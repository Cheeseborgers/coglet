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

## CogIR Lowering Tests

CogIR golden dumps cover executable entry identity, structured-CFG, data/address,
explicit wrapping lowering, and native-C variadic default promotions. The entry
golden demonstrates that a source `main::() -> i32` lowers to the distinguished
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
coverage executes `main::() -> i32 { return 42; }`, rejects the obsolete
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

The Stage 1-6 LLVM tests emit IR through the compiler, which means every success
case has already passed LLVM's module verifier inside the backend. When `clang`
is available, those compatibility/regression harnesses also compile the emitted IR
and check executable status. Stage 7 adds direct native-executable tests that use
`--backend llvm`: the compiler verifies LLVM IR, emits an object with the native
`TargetMachine`, links it, and executes the result without routing the object path
through textual `.ll` plus `clang`.
Stage 1 coverage remains for scalar conditional CFG/direct calls, the resolved
Coglet entry adapter, and ordered module initialization. Stage 2 adds checked and
wrapping integer arithmetic, division/remainder traps, bitwise operations, checked
shift counts, integer-backed enums, integer conversions, and Fibonacci loop
execution. Stage 3 adds native target triple/data-layout emission, pointer
comparison and reinterpretation, field/array/pointer addressing, ordinary struct
and array values, aggregate copies/arguments/returns, aggregate globals, and
negative coverage that keeps `#repr(c)` and volatile whole-aggregate behavior out
of the subset until their ABI/access semantics are implemented explicitly. Stage
4 adds floating arithmetic/comparisons and checked float/integer conversions,
integer/boolean/enum switches, indirect native Coglet function values/calls, and
direct CogIR coverage for the explicit trap terminator. Stage 5 links emitted
LLVM IR against an independently compiled C support library to exercise exact C
scalar aliases, SysV extension attributes, callbacks and returned function
pointers, direct and indirect C variadics/default promotions, explicit Win64
calling convention lowering, incomplete/volatile pointers, and external symbol
overrides. Stage 6 reuses the independently compiled C support library for
represented structs, nested structs, arrays, enums, unions, packed/explicitly
aligned objects, aggregate callbacks, represented globals, and addressable C
`_Bool` objects. The x86-64 suite exercises both SysV direct/register and
`byval`/`sret` paths plus Win64 small and indirect aggregate rules. A negative
regression keeps ordinary Coglet `bool*` distinct from `c_bool*`, and volatile
whole-aggregate access remains explicitly unsupported. Stage 7 direct-link
coverage includes native Coglet `main::() -> i32`, exact C scalar ABI linkage,
and a represented aggregate C ABI call/return through the independently compiled
support library. Stage 7 also includes an `-O0` mutable-global executable case
that requires position-independent object code on hosts whose compiler driver
defaults to PIE, preventing optimization from masking relocation-policy bugs.
Stage 8 adds explicit `-O0`, checked integer execution at
`-O1`, represented aggregate C interop at `-O2`, volatile C pointer behavior at
`-O3`, an optimized textual-IR transformation check, and a direct-link `-O3`
checked-overflow trap. Driver regressions also reject nonzero optimization on the
host-C executable path and unsupported optimization levels. Stage 9 adds `-g`
coverage for compile units/source locations, source functions, globals, parameters,
locals, arrays/structs/enums/pointers, represented C union metadata, optimized
`-g -O2` IR, and direct native `-g` execution at both O0 and O3. The native debug
harness additionally checks that the linked executable contains the expected source
filename, so merely accepting `-g` cannot satisfy the regression. Negative driver
coverage rejects host-C executable debug requests and `-g` requests with no LLVM
output. Compiler-generated storage slots and the synthetic process-entry adapter are
checked not to masquerade as Coglet source variables/code. The float suite includes
NaN/infinity, signed/unsigned range boundaries, and narrowing behavior so LLVM
conversion instructions are not used before Coglet's checked-cast guards.

Run only these tests with:

```bash
ctest --test-dir cmake-build-debug -L backend.llvm --output-on-failure
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
./cmake-build-debug/check_semantics \
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
