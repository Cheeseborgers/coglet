# Coglet

## Overview

Coglet is an experimental statically typed systems programming language with a modern, name-first syntax inspired by languages such as Jai.

Its early semantics intentionally remain close to C: explicit control, manual memory management, predictable layout, and no garbage collector. The compiler is being developed in small stages with correctness and understandable architecture taking priority over feature count.

The long-term objective is a compiler capable of compiling itself. Language design can then evolve beyond C where improvements provide clear value without sacrificing performance, control, or implementation clarity.

## Current Goals

- Build a complete, understandable compiler.
- Keep language semantics explicit and predictable.
- Maintain a clean separation between parsing, semantic analysis, runtime design, and backend lowering.
- Reach a stable self-hosting implementation.
- Add features incrementally without accumulating special-case behavior.

## Design Philosophy

- Statically typed.
- Manual memory management.
- No garbage collector.
- Explicit control over memory and performance.
- Familiar low-level semantics with cleaner syntax.
- Correctness and simplicity before advanced features.
- Semantic facts should be explicit enough to support diagnostics, testing, tooling, interpretation, and any later backend design.

## Current Language Features

### Declarations

```c
value: i32 = 42;
other := 42;
name: u8[6] = "hello";
```

Supported declaration behavior includes:

- explicit types
- type inference with `:=`
- name-first declaration syntax
- compile-time constants
- contextual array and string initializers
- rejection of `void` as a stored value type
- concrete default types for inferred mutable numeric storage
- adaptable compile-time numeric constants

### Functions

```c
add::(a, b: i32) -> i32 {
    return a + b;
}
```

Supported features include:

* typed parameters
* grouped parameter declarations
* optional default parameter values
* explicit return types
* omitted return types defaulting to `void`
* argument type checking
* unified reachability and non-void fallthrough checking
* unreachable-statement diagnostics
* nested function declarations

Nested functions do not currently support closure capture.
They may access visible globals, constants, types, and function declarations, but cannot read or modify
locals and parameters belonging to an enclosing function.

The frontend also supports declaration-only C-linkage functions:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;

#extern(c)
malloc::(size: c_size) -> opaque*;

#extern(c, name="SDL_CreateWindow")
create_window::(title: readonly c_char*) -> opaque*;

#extern(c, call=win64)
platform_probe::(value: c_int) -> c_int;
```

`#extern(c)` declarations are top-level, have no Coglet body, and currently
accept the scalar/raw-pointer ABI subset plus explicitly represented C
aggregates/enums and native `cfn` callback types. `name="..."` optionally changes the
external symbol without changing the Coglet identifier. The native C scalar
family is available through transparent aliases such as `c_char`, `c_short`,
`c_int`, `c_long`, `c_longlong`, their unsigned forms, `c_size`, `c_bool`,
`c_float`, and `c_double`, selected from the native C ABI used to build the
compiler. C floating aliases require native IEEE binary32/binary64 compatibility.
The initial host-C backend can now compile a
deliberately small executable subset and resolve direct external C calls through
the native `cc` toolchain. Direct string literals may bind to `readonly c_char*`
parameters of `#extern(c)` functions without enabling general array-to-pointer
decay. `#repr(c)` provides a C-compatible aggregate layout contract for
top-level structs with supported scalar/raw-pointer fields, fixed-size arrays of
supported field types, and nested `#repr(c)` structs by value; those structs may
cross extern parameters and returns by value. By-value layout dependencies,
including struct dependencies reached through array fields, are cycle-checked and
need not follow source order. Native C callbacks use explicit `cfn(...) -> T`
types, and Coglet-defined callbacks opt into the C ABI with top-level `#repr(c)`
functions. `#extern(c, call=...)`, `#repr(c, call=...)`, and
`cfn(call=..., ...)` support explicit `cdecl`, `stdcall`, `sysv64`, and `win64`
calling-convention contracts; calling convention is part of callback type
identity. Raw pointers also support independent `volatile` qualification, including `readonly volatile` combinations, with monotonic qualifier conversion and native C `volatile` lowering. The host-C backend emits real function-pointer typedefs and supports C
calling back into those Coglet functions. C-variadic extern declarations and
variadic `cfn` types are supported for the current scalar/pointer ABI subset,
with standard C default argument promotions performed by the native compiler.
Complete represented structs/unions also support `#repr(c, packed)`,
`#repr(c, align=N)`, or both. `align=N` is a positive-power-of-two minimum
alignment request; the current host-C backend lowers these controls with guarded
GNU-compatible layout attributes and refuses unsupported host compilers rather
than silently changing ABI layout. Body-less `#repr(c) Name::struct;`
declarations model incomplete/opaque named C structs: they may cross C
boundaries only through raw pointers, while by-value storage, construction,
dereference/indexing, and field access are rejected. Explicit cross-target ABI
selection and native Coglet variadics remain future work.

The compiler version is tracked from one CMake project version and is available
without compiling an input file:

```sh
coglet --version
# coglet 0.1.0
```

The generated public version header exposes the same major/minor/patch/string
metadata for future compatibility and deprecation diagnostics. Version tracking
does not yet imply a source compatibility or deprecation policy.

### Modules and Imports

Multiple source files still compile into one CogIR compilation unit, but the
frontend supports hierarchical named semantic namespaces:

```c
// lib/std/math.cog
module std.math;
export add::(a, b: i32) -> i32 { return a + b; }
```

```c
// main.cog
import std.math;
main::() -> i32 { return std.math.add(20, 22); }
```

Module names are absolute dotted identifier paths. The same dot is used for
module qualification and ordinary member selection: `std.math.add`,
`std.math.Pair`, `std.math.Mode.Red`, `state.point.x`. Semantic resolution uses
the longest module prefix visible in the current file, so no second namespace
operator is required. Importing `std.math` does not implicitly import `std` (or
vice versa), and a lexical value still shadows a module name in expression
lookup.

The command-line compiler performs deterministic source discovery for missing
imports. A module name maps dots to path separators, so `import std.math;`
searches first for `std/math.cog` beside the importing file and then under each
repeated `-I` module search root in command-line order. If the imported name is
`std` or begins with `std.`, one compiler-configured standard-library module root
is consulted last. The default configured root is the install-prefix-relative
`${CMAKE_INSTALL_LIBDIR}/coglet` location (normally `/usr/local/lib/coglet`), so
`std.math` maps to `<stdlib-root>/std/math.cog`. Packagers may change
`COGLET_STDLIB_INSTALL_DIR` at CMake configure time, while `--stdlib-root <dir>`
provides a per-invocation development/toolchain override and
`--print-stdlib-root` reports the configured default. Non-`std` imports never
fall back to this compiler-owned root.

The first existing candidate is loaded, its imports are discovered transitively,
and the same physical file is not loaded twice. Explicit command-line inputs are
parsed first and still take precedence: if one already contributes to
`module std.math`, no canonical `std/math.cog` is auto-loaded for that import.
Additional files for a multi-file module remain explicit until a manifest/package
layer exists. User/importer sources and `-I` roots intentionally precede the
standard-library fallback, so local or development copies may override installed
`std.*` modules without changing compiler binaries.

Files without a module declaration belong to the root namespace; multiple files
may contribute to the same named module. Imports are file-scoped. Declarations
in named modules are private by default; the contextual top-level `export` prefix
exposes a declaration to importing files. Same-module code sees private
declarations normally, including through explicit current-module qualification.
Imported files may qualify only exported functions, globals, constants, and
nominal types. Qualified globals preserve ordinary lvalue/addressability
semantics, while qualified constants remain compile-time values and may be used
in constant-expression contexts. Module data declarations are visible to
function bodies regardless of physical input-file order; compile-time constant
dependencies are resolved lazily and cycles are diagnosed.

Import cycles are allowed because imports currently affect compile-time
visibility only; top-level runtime initialization remains in physical input
order. Only root-namespace `main::() -> i32` is the executable entry. Exported
APIs may not expose private nominal types through function signatures,
globals/constants, or exported struct fields. Package manifests, automatic
multi-file package membership, the actual standard-library module contents, and
separate compilation remain future work. Discovery does not imply runtime
dependency ordering: explicit inputs retain command-line order and discovered
files are appended in deterministic first-discovery order to the existing single
module initializer.

For the current executable slice:

```sh
coglet program.cog -o program
coglet main.cog math.cog util.cog -o program
coglet main.cog -I lib -o program
coglet main.cog --stdlib-root /path/to/coglet/modules -o program
coglet --print-stdlib-root
coglet program.cog -o program --backend llvm
coglet program.cog -o program --backend llvm -O2
coglet program.cog -o program --backend llvm -O2 -g
coglet program.cog -o program -L/path/to/lib -lfoo
coglet program.cog -o program -L /path/to/lib -l foo
coglet program.cog --emit-c program.c
coglet program.cog --emit-llvm program.ll
coglet program.cog --emit-llvm optimized.ll -O2
coglet program.cog --emit-llvm debug.ll -g
coglet program.cog --emit-asm program.s
coglet program.cog --emit-asm optimized.s -O3
coglet program.cog --emit-asm debug.s -O2 -g
coglet program.cog -o program --backend llvm --emit-asm program.s -O2
```

`-I` adds a Coglet module source-search root for automatic `import` discovery;
it is unrelated to native C header search. Both `-I dir` and `-Idir` are accepted,
and repeated roots are searched left-to-right after the importing file's directory.
Dotted module names map directly to relative source paths (`std.math` ->
`std/math.cog`). After those user-controlled roots, `std`/`std.*` imports alone may
fall back to the compiler's configured standard-library module root.
`--stdlib-root <dir>` (or `--stdlib-root=<dir>`) replaces that fallback for the
current invocation; it does not alter ordinary package lookup or C linker paths.

`-L` adds a native library search directory and `-l` requests a native library.
Both joined and split spellings are accepted, both options may be repeated, and
they are valid only when `-o` requests an executable link step. They are passed
directly to the selected backend's native linker/compiler driver without invoking
a shell. Executable output defaults to the host-C bootstrap backend. When LLVM
support is enabled, `--backend llvm` lowers and verifies LLVM IR, emits a
position-independent native object through LLVM's `TargetMachine`, then invokes
the host `cc` driver only for the final platform link/CRT step. Executable-object
PIC is backend/toolchain policy so hosts whose compiler driver defaults to PIE do
not require optimization or linker-specific `-no-pie` workarounds. Textual LLVM IR
remains available through `--emit-llvm`, and `--emit-asm` asks the same
verified/optimized target-machine pipeline to write native target assembly without
invoking an external assembler. Assembly output uses LLVM's PIC relocation model,
matching the native executable path and remaining suitable for host toolchains that
default to PIE. It may be requested on its own or alongside `--backend llvm -o` to
retain an assembly view while also producing the executable. LLVM output accepts
`-O0`, `-O1`, `-O2`, and `-O3`; `-O0` is the default and preserves the unoptimized
LLVM-IR path, while `-O1` through `-O3` run LLVM's corresponding default
new-pass-manager pipeline before IR/assembly/object emission and select the matching
target code-generation optimization level.
Nonzero optimization levels are currently rejected for host-C executable output
rather than being silently ignored. LLVM output also accepts `-g`, which emits
source-level debug metadata from frozen CogIR source provenance for source
functions, locations, globals, parameters, locals, and the currently supported
runtime types. Compiler-generated local-storage slots are not exposed as source
variables, and the synthetic process-entry adapter is deliberately left without a
fake Coglet source location. `-g` composes with `-O0` through `-O3`; optimized
debugging follows LLVM's normal optimized-code behavior. Host-C executable output
rejects `-g` rather than silently claiming to provide equivalent debug metadata.

Coglet executables require a source-top-level `main::() -> i32`. The `c_*`
types are C-interoperability types and are not part of the language-level entry
contract. The host-C backend adapts the resolved CogIR entry to the C process ABI
by emitting a C `int main(void)` wrapper. Running `coglet` with only the input
filename still performs frontend parse/semantic checking without requesting
backend generation.


### Types

Primitive and built-in types:

- `bool`
- `i8`, `i16`, `i32`, `i64`
- `u8`, `u16`, `u32`, `u64`
- `f32`, `f64`
- `void`

`null` is a dedicated contextual pointer literal. It is not integer zero and is
not a user-declarable storage type.

Compound and declared types:

- mutable and readonly raw nullable object pointers
- mutable and readonly opaque raw pointers
- fixed-size arrays
- nominal structs
- nominal enums
- function types

`void` is valid as a function return type, but not as a variable, constant, parameter, struct field, pointer element, or array element type in the current language.

### Expressions

Supported expression forms include:

- numeric, boolean, character, `null`, and contextual string literals
- identifiers
- arithmetic operators
- bitwise operators (`&`, `|`, `^`, `~`)
- shift operators (`<<`, `>>`)
- equality and ordered comparisons
- logical operators (`&&`, `||`)
- unary negation, logical negation, and bitwise complement
- function calls
- field access
- array and pointer indexing
- checked casts, explicit raw-pointer reinterpretation, and integer truncation
- compiler-provided wrapping integer operations
- struct initializers
- contextual array literals

### Mutation Statements

Assignment, compound assignment, and increment/decrement are statement-only operations.

```c
x = 1;
x += 1;
x &= mask;
x <<= count;
x++;
```

They are valid as standalone statements and in `for` post clauses.

They are not value-producing expressions:

```c
y := (x = 1);       // invalid
takes_i32(x += 1);  // invalid
return x++;         // invalid
```

Mutation targets must denote assignable storage.

Valid targets include mutable variables and fields/indexes whose base is assignable.

Invalid targets include constants, enum members, and fields or indexes derived from temporary values.

### Definite Assignment

Local variables are not implicitly initialized:

```c
value: i32;
```

A variable may be read only when semantic analysis can prove that it has been initialized on every
reachable incoming path.

A direct whole-variable assignment initializes it:

```c
value: i32;

value = 10;

return value;
```

Parameters and variables declared with initializers begin initialized.

Compound assignment and increment/decrement require prior initialization because they read
the previous value:

```c
value: i32;

value += 1; // invalid
value++;    // invalid
```

Assigning an entire struct or array initializes that variable. Assigning only a field, element,
pointee, or pointer-indexed location does not initialize the complete base variable.

Taking the address of an uninitialized local is rejected.

Branch merging is reachability-aware. An unreachable branch does not weaken a branch that continues,
and non-exhaustive switches include an implicit no-match path.

Loop analysis is conservative because a loop may execute zero times. Initialization performed only
inside a loop is not generally available afterward.


### Void-Returning Calls

A call to a function returning `void` is valid when its result is discarded:

```c
does_nothing();
```

It is invalid in a value-required context:

```c
x := does_nothing();
takes_i32(does_nothing());
return does_nothing();
does_nothing() + 1;
```

### Raw Object Pointers

Coglet provides mutable and readonly raw object pointers:

```c
value: i32 = 10;

pointer: i32* = &value;
view: readonly i32* = pointer;
```

`T*` is a raw pointer that grants mutable access to `T`.
`readonly T*` grants read access but does not permit mutation through that
pointer:

```c
read: i32 = *view;  // valid
*view = 20;         // invalid
view[0] = 20;       // invalid
```

Readonly access is shallow and access-based. It does not guarantee that the
underlying object never changes because the same object may remain reachable
through another mutable pointer.

Both pointer forms are nullable, non-owning, and unchecked for lifetime,
alignment, bounds, aliasing, and dangling values.

A mutable pointer may adapt implicitly or explicitly to the corresponding
readonly pointer:

```c
view: readonly i32* = pointer;
other := cast(readonly i32*, pointer);
```

Readonly access cannot implicitly or explicitly become mutable access.
Dereference, pointer indexing, field access, and address-of preserve the
relevant access permission:

```c
readonly_again: readonly i32* = &*view;
```

Pointers with the same immediate pointee type may be compared even when one is
mutable and the other readonly. Comparison does not grant additional
permissions:

```c
pointer == view;
pointer != null;
view == null;
```

Nested pointer qualification is strict. Readonly access is not introduced
recursively through multiple pointer layers.

`null` is the only source-level null-pointer value:

```c
pointer: i32* = null;          // valid
view: readonly i32* = null;    // valid
pointer: i32* = 0;             // invalid
```

An explicit `null`-to-pointer cast may provide either concrete pointer type:

```c
mutable_null := cast(i32*, null);
readonly_null := cast(readonly i32*, null);
```

### Opaque Raw Pointers

Opaque handles use `opaque*` and `readonly opaque*`. They are nullable and
address-like but cannot be dereferenced or indexed:

```c
handle: opaque* = null;
view: readonly opaque* = handle;
```

Additional pointer layers compose normally, so `opaque**` is a pointer to an
`opaque*` slot and may be dereferenced once.

Typed and opaque raw pointers do not implicitly convert. Crossing the boundary
requires `reinterpret`, which preserves the address representation and cannot
discard readonly access:

```c
pointer: i32* = get_pointer();
handle := reinterpret(opaque*, pointer);
recovered := reinterpret(i32*, handle);
```

### Arrays

```c
values: i32[3] = [1, 2, 3];
```

Arrays have a fixed compile-time size that is part of the type.

Supported array behavior includes:

- indexing
- assignable indexed elements when the base is assignable
- contextual array literals
- contextual fixed-array `{0}` semantic-zero initialization

Typed mutable declarations may group names (`a, b: u64 = 0;`). The initializer is evaluated separately for each name, left-to-right.
- array assignment
- array arguments
- array return values
- struct fields containing arrays
- compile-time bounds checking for constant indexes

### String Literals

String literals are currently contextual initializers for fixed-size `u8` arrays.

```c
name: u8[6] = "hello";
```

The trailing null byte is included, so `"hello"` requires six bytes.

String literals currently work in expected-type contexts such as:

- variable declarations
- assignments
- function arguments
- return values
- struct field initializers

They are not yet inferred standalone expressions:

```c
name := "hello"; // invalid
"hello";         // invalid
```

### Structs

```c
Point :: struct {
    x: i32;
    y: i32;
}

p := Point {
    x = 1,
    y = 2,
};
```

Semantic analysis validates field names, duplicates, missing fields, and field initializer types.

Structs are nominal types: two separate struct declarations are distinct even
when they have identical fields or the same source-level name in different
scopes.

### Enums

```c
Color :: enum(u16) {
    Red,
    Green,
    Blue,
}
```

Supported enum behavior includes:

- explicit or default integer backing types
- implicit and explicit member values
- member range validation
- enum member access
- enum comparisons for equality
- enum switch cases and exhaustiveness analysis
- enum-to-integer casts
- compile-time integer-to-enum casts when the value names a declared member

Enums are closed, strongly typed, and nominal. A backing type defines
representation and range, but it does not make every backing-type value a
valid enum value. Separate enum declarations remain distinct even when they
share a backing type, members, or a shadowed source-level name. Runtime
integer-to-enum casts are currently rejected because checked runtime
conversion has not yet been implemented.

C ABI enums opt in explicitly with `#repr(c)` and an explicit native C integer backing alias:

```c
#repr(c)
CResult::enum(c_int) {
    Ok = 0,
    Error = 1,
}
```

The annotation fixes the external ABI representation but does not make the enum open: Coglet still accepts only declared member values and keeps normal enum exhaustiveness rules.

C callback pointers use an explicit structural type:

```c
#extern(c)
run_callback::(callback: cfn(c_int) -> c_int, value: c_int) -> c_int;

#repr(c)
identity::(value: c_int) -> c_int {
    return value;
}
```

`cfn` values are nullable and callable. Ordinary Coglet functions do not
implicitly adapt to them; `#repr(c)` marks a top-level Coglet function as using
the native C callback ABI.

## Numeric Semantics

Coglet keeps concrete numeric conversions explicit. Untyped literals adapt to
a concrete operation or destination type only when their exact value fits.

Ordinary signed and unsigned integer arithmetic is checked. Addition,
subtraction, multiplication, signed negation, increment/decrement, and their
compound forms require a representable result.

A known failure is a compile-time error. A runtime-dependent failure traps.
These rules are identical in debug and release builds, and unsigned arithmetic
does not wrap implicitly.

Integer division truncates toward zero, and remainder has the sign of the
dividend. Division or remainder by zero traps. Signed minimum divided or
remaindered by `-1` also traps.

Numeric `cast` is checked. Integer conversions must fit the destination.
Floating-point-to-integer conversion rejects NaN and infinity, truncates toward
zero, and then checks the destination range.

Coglet also provides explicit fixed-width alternatives:

```c
wrapping_add(left, right)
wrapping_sub(left, right)
wrapping_mul(left, right)
wrapping_neg(value)

truncate(TargetIntegerType, value)
```

Wrapping operations use modulo arithmetic at the concrete integer width.
`truncate` retains the low destination-width bits of an integer value and
interprets that bit pattern using the destination signedness. These explicit
operations do not alter the checked behavior of ordinary arithmetic or
`cast`.

`f32` and `f64` follow IEEE-754 binary32 and binary64 behavior, including
infinity, NaN, signed zero, and round-to-nearest with ties to even:

```c
1.0 / 0.0;   // positive infinity
-1.0 / 0.0;  // negative infinity
0.0 / 0.0;   // NaN
```

NaN compares unequal to itself, and ordered comparisons involving NaN are
false.

Bitwise operators are integer-only. Concrete operands for `&`, `|`, and `^`
must have the same type unless one operand is an adaptable untyped integer
constant that fits the concrete type. Signed bitwise operations use a defined
fixed-width two's-complement representation.

For shifts, the left operand determines the result type and bit width. Every
count must satisfy `0 <= count < bit_width`. A known invalid count is a
compile-time error, while a runtime-dependent invalid count traps. Counts are
never masked.

Left shift is a fixed-width bit-pattern operation: bits shifted beyond the
width are discarded. Unsigned right shift zero-fills, while signed right shift
is arithmetic and sign-extending.

Coglet intentionally gives bitwise operators higher precedence than equality
and ordered comparisons. Therefore:

```c
flags & mask == 0;
```

parses as `(flags & mask) == 0`, avoiding C's surprising precedence rule.

### Control Flow

Supported control flow includes:

* `if` / `else`
* `while`
* `for`
* `switch`
* `break`
* `continue`
* `return`
* lexical block scopes

`if`, `else`, `while`, and `for` accept either a braced block or one unbraced
statement. Unbraced bodies still introduce a lexical scope:

```c
if n == 0
    return 0;

while i < limit
    i++;
```

`for` supports the compact Coglet header with optional parentheses and a
parenthesized three-clause form with a loop-scoped initializer:

```c
for i < limit : i++ {
    work(i);
}

for (i < limit : i++)
    work(i);

for (i: u32 = 0; i < limit; i++) {
    work(i);
}
```

The three-clause form permits omitted clauses, including `for (;;)`, and its
initializer is visible through the condition/body/post but not after the loop.

Semantic analysis uses a unified reachability model for:

* definite assignment;
* branch and switch merging;
* `return`, `break`, and `continue`;
* unreachable-statement diagnostics;
* non-void function fallthrough checking.

A non-void function is valid when normal control flow cannot reach the end of its body.
This includes functions that return on every continuing path and functions containing a provably
non-terminating literal-true loop with no reachable `break`.

Switch analysis includes:

* switch-expression type checking;
* compile-time case validation;
* duplicate runtime-value detection;
* duplicate-`default` detection;
* Boolean and enum exhaustiveness;
* independent definite-assignment flow for every case.

Exhaustiveness is based on successfully validated runtime values. Invalid cases do not contribute coverage, and aliased enum members with the same value require only one corresponding case.


Switch analysis includes case type checking, compile-time case validation, duplicate-case detection, duplicate-default detection, and enum exhaustiveness checks.

## Semantic Model

Semantic analysis records expression information in a side table.

Facts include:

- resolved type;
- resolved symbol, when applicable;
- value category: lvalue, rvalue, or none;
- storage access: writable, readonly, or none.

Value category and storage access are separate. A readonly dereference still
identifies storage and is therefore an lvalue, but it is not writable.

Examples:

```text
mutable variable expression:
    type = variable type
    category = lvalue
    access = writable

readonly pointer dereference:
    type = pointee type
    category = lvalue
    access = readonly

numeric literal or adaptable constant:
    type = untyped-int or untyped-float
    category = rvalue
    access = none

void-returning call:
    type = void
    category = none
    access = none

successful assignment statement:
    type = none
    category = none
    access = none
```

This distinction is used to enforce assignability, readonly access, and
value-required contexts.

## Memory Model

Coglet currently follows a traditional C-style memory model.

- No garbage collector.
- Manual memory management.
- Low-level pointer access.
- Predictable runtime behavior.
- Explicit control remains a core design principle.

Higher-level facilities may be added later, but they should not obscure ownership, allocation, or performance costs.

## Current Status

The parser and semantic analyzer support a substantial core language. The
semantic-information verifier walks successful programs in source order and
checks table completeness, duplicate/orphan entries, value categories, storage
access, symbol associations, and concrete variable/parameter types. An
optional diagnostic flag prints the semantic table deterministically.

Recently completed work includes:

- canonical shared semantic types for built-in scalars;
- dedicated `null` semantics with no integer-zero pointer conversion;
- nominal declaration identity for structs and enums;
- restricted equality and ordered-comparison operand categories;
- checked known integer zero-divisor diagnostics;
- integer-only bitwise and shift operators with defined fixed-width semantics;
- build-mode-independent checked runtime scalar semantics;
- explicit wrapping integer builtins;
- explicit truncating integer conversion;
- exact constant arithmetic and representability checks;
- IEEE-754 constant behavior for `f32` and `f64`;
- closed enum value sets and checked enum conversions;
- definite-assignment and unified reachability analysis;
- mutable and readonly raw-pointer access;
- safe mutable-to-readonly pointer adaptation;
- access-preserving dereference, indexing, fields, and address-of;
- separate semantic facts for storage identity and write permission;
- readonly-pointer compatibility and semantic-info verification;
- rejection of unsupported nested-function captures;
- native `#repr(c)` union ABI layout for FFI carrier values.

The host-C backend is the bootstrap executable path for the current CogIR
contract. It consumes only frozen CogIR, covers every current `CogIrOp`, and is
built as a separate backend library beneath the frontend/semantic/CogIR core.
This keeps C implementation details out of the language boundary. An optional
LLVM Stage 9 backend now consumes the same frozen module through its own backend
library. It emits verifier-checked LLVM IR with an explicit native target triple
and data layout, can emit native assembly or an object with LLVM target APIs, and
can link the object into an executable through a separate host linker-driver
boundary. Assembly emission is a direct TargetMachine output and can be retained
alongside an executable build. It can also run LLVM's target-aware default
`O1`/`O2`/`O3` optimization pipelines after
initial verification and verifies the transformed module again before output. With
`-g`, it derives LLVM source-level debug metadata only from frozen CogIR-owned
source files, spans, declaration/type metadata, exact C ABI spelling where storage
representation matters, and explicit local-slot provenance; no frontend-lifetime
object is retained for debugging. Its
implemented lowering covers native scalar/CFG and integer semantics, ordinary Coglet
memory and aggregates, floating-point operations and conversions, switch/trap
terminators, indirect native Coglet function values/calls, and the C scalar,
pointer, callback/function-pointer, variadic, represented-object, and aggregate
ABI surface. On x86-64 it classifies complete `#repr(c)` structs/unions for SysV
and Win64 calling conventions, including direct register coercions, `byval`,
hidden `sret`, packed/explicit alignment, nested arrays/unions, and C `_Bool`
object storage. Ordinary Coglet `bool*` is intentionally not interchangeable with
`c_bool*`; exact C object spelling must be present when the storage layouts differ.

## Roadmap

Near-term compiler work is backend-focused:

1. Extend represented C aggregate classification beyond the current x86-64 SysV/Win64 target slice when cross-target selection is introduced.
2. Extend the hierarchical module/import layer with package manifests and actual standard-library modules on top of the configured `std.*` search root, then define the runtime/standard-library boundary needed for self-hosting.
3. Continue improving diagnostics, tests, and documentation.

## License

TBD
