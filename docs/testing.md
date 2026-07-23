# Testing

Coglet uses CTest to run lexer, parser, semantic, constant-evaluation, and semantic-information tests.

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
* valid semantic programs;
* invalid semantic diagnostic snapshots;
* compile-time constant-value oracles;
* semantic-information snapshots and verification.

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
./tests/get_expected.sh
```

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
* casts;
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
* non-terminating literal-true loops;
* nested loops;
* nested-function flow ownership;
* rejection of unsupported nested-function captures;
* semantic-information recording for rejected reads.

Tests for another semantic rule may need to initialize arrays or structs explicitly so that 
definite-assignment diagnostics do not mask the behavior the test is intended to exercise.

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

## Known Parser Harness Issue

There is an older parser test involving an integer literal whose magnitude exceeds `u64`.

The compiler correctly rejects that source, but the parser snapshot harness may still expect the 
success status used by ordinary AST snapshot tests. If the test remains failing, 
do not classify it as a definite-assignment or semantic-analysis regression.

The parser test harness should eventually distinguish:

* successful parser snapshots;
* expected parser failures and their diagnostics;
* their different process exit statuses.
