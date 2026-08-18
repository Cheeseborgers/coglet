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
value: s32 = 42;
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
- hexadecimal floating-point literals with binary `p`/`P` exponents

### Functions

```c
add::(a, b: s32) -> s32 {
    return a + b;
}
```

Supported features include:

* typed parameters
* grouped parameter declarations
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
export add::(a, b: s32) -> s32 { return a + b; }
```

```c
// main.cog
import std.math;
main::() -> s32 { return std.math.add(20, 22); }
```

Imports may use a file-local alias when the canonical path is too repetitive:

```c
import std.math as math;
import std.io as io;

main::() -> s32 {
    io.print_s32(math.add(20, 22));
    io.newline();
    return 0;
}
```

The alias changes only local qualification; module discovery and identity still
use the canonical name (`std.math`, `std.io`).

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
order. Only root-namespace `main::() -> s32` is the executable entry. Exported
APIs may not expose private nominal types through function signatures,
globals/constants, or exported struct fields. Package manifests, automatic multi-file package membership, and separate compilation remain future work. Coglet ships ordinary-source `std.math`, `std.io`, `std.mem`, `std.array`, and `std.pool` modules beneath `stdlib/std/`; installed builds copy the `std` source tree and the small runtime implementation beneath the configured standard-library root. Discovery does not imply runtime
dependency ordering: explicit inputs retain command-line order and discovered
files are appended in deterministic first-discovery order to the existing single
module initializer.


The first shipped standard module is intentionally small:

```c
import std.math;

main::() -> s32 {
    if std.math.gcd_u64(84, 30) != 6
        return 1;

    return std.math.max(20, 22);
}
```

`std.math` provides adaptable hexadecimal floating-point constants including `pi`, `tau`, `e`, angle-conversion factors, and common derived constants; concrete integer helpers `abs_s32`/`gcd_u64`; generic `min<T: ordered>`/`max<T: ordered>`/`clamp<T: ordered>`; floating game/application helpers such as `lerp`/`smoothstep`; generic `Vec2<T>`/`Vec3<T>`/`Vec4<T>` numeric vectors with constructors, component arithmetic, dot/cross and distance helpers; floating `Quat<T>`, `Mat3<T>`, and `Mat4<T>` transform types with quaternion interpolation/rotation and TRS composition; and runtime-backed `f32`/`f64` square-root, trigonometric, inverse-trigonometric, rounding, and floating-remainder functions. The constants remain compile-time `untyped-float` values, so `f32` and `f64` contexts materialize the appropriate precision without a use-site cast. See `docs/stdlib.md` for the API, semantics, installation layout, and stdlib testing workflow. Known implementation limitations and follow-up work are tracked separately in `docs/known_shortcomings.md`.

`std.io` provides runtime-backed byte-view and scalar output. `std.mem` provides explicit allocator values, the process heap allocator, typed allocation, growable arenas, caller-buffer fixed arenas, scoped scratch checkpoints, and a guard/poison/tracking `DebugAllocator`; `std.array` stores an allocator inside each growable `Array<T>`, while `std.pool` provides fixed-capacity stable slots with generation-checked handles. The public modules remain ordinary Coglet source. During CogIR freeze, reserved `coglet_rt_*` extern declarations are summarized into runtime requirements; executable links then compile only the required `<stdlib-root>/runtime/coglet_runtime_{io,math,mem}.c` components.

```c
import std.io;

main::() -> s32 {
    std.io.println("hello from Coglet");
    std.io.print_s32(42);
    std.io.newline();
    return 0;
}
```

A growable array remains explicit about ownership and cleanup:

```c
import std.array as array;
import std.mem as mem;

main::() -> s32 {
    values := array.Array::<s32>.new(mem.heap());
    defer values.deinit();

    values.push(10);
    values.push(20);

    view := values.as_slice();
    view[1] = 25;

    return 0;
}
```


For allocation-heavy game code, the same allocator handle can describe heap, arena,
or fixed-buffer storage:

```c
import std.mem as mem;
import std.pool as pool;

storage: u8[64 * 1024] = {0};
fixed := mem.FixedArena.from_buffer(storage);
defer fixed.deinit();

frame := mem.Arena.new(mem.heap());
defer frame.deinit();

{
    scratch := mem.Scratch.begin(&frame);
    defer scratch.deinit();
    temporary := scratch.allocator();
    // allocations made through temporary are rewound at scope exit
}

entities := pool.Pool::<Entity>.with_capacity(mem.heap(), 1024);
defer entities.deinit();
```

For allocator diagnostics, wrap the allocator used by a subsystem rather than changing the container API:

```c
import std.array as array;
import std.mem as mem;

tracker := mem.DebugAllocator.new(mem.heap());
defer tracker.deinit();

alloc := tracker.allocator();
values := array.Array::<s32>.new(alloc);
defer values.deinit();

// Live allocation/byte counters and guard validation are queryable.
if !tracker.check()
    return 1;
```

`DebugAllocator` places fixed guards around live allocations, fills newly allocated storage with `0xCD`, fills storage with `0xDD` before returning it to the parent allocator, and can report outstanding blocks using stable allocation IDs. It does not yet attach source file/line metadata because Coglet has no source-location intrinsic in ordinary allocator calls.

`Array<T>` and `Pool<T>` currently require `T: copyable`; resource-valued elements
are rejected until the container layer has element-wise move/destruction semantics.

`Array<T>` and `std.mem.Arena` are move-only `resource` values. Existing owners
cannot be copied; ownership transfer is explicit with `move`. Cleanup remains
explicit and normally pairs naturally with `defer`:

```c
values := array.Array::<s32>.new(mem.heap());
defer values.deinit();

// Passing ownership requires an explicit transfer.
consume(move values);
```

After a successful move, the source local is treated as uninitialized and may not
be read until assigned a fresh resource. Coglet intentionally does not perform
borrow/lifetime checking for pointers or slices; borrowed views can still be
invalidated by reallocation, arena reset, or resource destruction.

### Build and run from a source checkout

Configure once with the native C toolchain you want Coglet executables to reuse:

```sh
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
```

On Linux this normally means GCC or Clang. On Windows, run CMake from an
environment where the selected MSVC/Clang/GNU native toolchain is available; an
MSVC C frontend must provide C11 mode. The configured compiler path is baked into
Coglet's native executable toolchain layer, so a later `coglet ... -o ...` does
not guess which compiler driver to execute.

When developing from the source tree, pass the source stdlib root explicitly:

```sh
./cmake-build-debug/coglet main.cog --stdlib-root ./stdlib -o program
./program
```

On Windows the executable normally has the platform `.exe` suffix. Installed
builds use the configured stdlib root automatically; `coglet --print-stdlib-root`
shows it. Runtime-backed modules require the matching `runtime/coglet_runtime_io.c`,
`runtime/coglet_runtime_math.c`, and/or `runtime/coglet_runtime_mem.c` component
beneath that root. Pure programs that do not declare reserved runtime symbols do
not acquire a runtime link dependency. `runtime/coglet_runtime.c` remains a
compatibility umbrella for consumers that intentionally compile the v0 runtime
as one translation unit; the Coglet driver links components directly.

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
a shell. CMake records the native C compiler used to build Coglet, and both
executable backends reuse that configured toolchain rather than assuming a Unix
`cc` command. Native Linux builds spawn it with the POSIX process API; native
Windows builds use the CRT process API and MSVC-style arguments when CMake selected
an MSVC-compatible frontend. The current target model remains host-only: this is
intended for native Linux and Windows builds on x86-64 and AArch64, not a cross-
compilation CLI.

Executable output defaults to the host-C bootstrap backend. When LLVM support is
enabled, `--backend llvm` lowers and verifies LLVM IR, emits a position-independent
native object through LLVM's `TargetMachine`, then invokes the configured host C
driver only for the final platform link/CRT step. Executable-object
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

Coglet executables require a source-top-level `main::() -> s32`. The `c_*`
types are C-interoperability types and are not part of the language-level entry
contract. The host-C backend adapts the resolved CogIR entry to the C process ABI
by emitting a C `int main(void)` wrapper. Running `coglet` with only the input
filename still performs frontend parse/semantic checking without requesting
backend generation.


### Types

Primitive and built-in types:

- `bool`
- `s8`, `s16`, `s32`, `s64`
- `u8`, `u16`, `u32`, `u64`
- target-sized integer aliases `isize` and `usize`
- `f32`, `f64`
- `void`

`usize`/`isize` resolve to the unsigned/signed fixed-width integer matching the
selected target pointer width. The built-in type queries `size_of(T)` and
`align_of(T)` return `usize`; ordinary generic calls keep the `name::<T>(...)`
syntax.

`null` is a dedicated contextual pointer literal. It is not integer zero and is
not a user-declarable storage type.

Compound and declared types:

- mutable and readonly raw nullable object pointers
- mutable and readonly opaque raw pointers
- fixed-size arrays
- mutable `T[]` and readonly `readonly T[]` slices
- nominal structs, including monomorphized generic structs
- nominal enums
- function types

`void` is valid as a function return type, but not as a variable, constant, parameter, struct field, pointer element, or array element type in the current language.

Top-level ordinary Coglet functions and structs may declare compile-time type
parameters with the shared `::<...>` syntax. Generic functions support
argument-based inference; generic struct types are explicit, for example
`Pair::<s32, f32>` or `Vec2::<f32>`. Both may use the closed builtin constraints
(`integer`, `signed_integer`, `unsigned_integer`, `floating`, `numeric`,
`ordered`). Every specialization is resolved and checked before CogIR, so both
backends see only ordinary concrete functions and nominal struct layouts.

Ordinary complete Coglet structs may also declare methods directly beside their
fields. A first parameter named `self` makes the declaration an instance method;
a declaration without `self` is an associated function. `Self` names the owning
concrete struct inside the method signature and body:

```c
Vec2::<T: numeric> struct {
    x: T;
    y: T;

    new::(x: T, y: T) -> Self {
        return Self { x = x, y = y };
    }

    sum::(self: readonly Self*) -> T {
        return (*self).x + (*self).y;
    }
}

point := Vec2::<f32>.new(1.0, 2.0);
total := point.sum();
```

Methods are frontend sugar only: semantic analysis inserts the receiver and
lowers both forms to ordinary concrete function calls before CogIR.

Structs may also opt into a deliberately small arithmetic-operator surface by
mapping operators to existing by-value methods:

```c
Vec2::<T: numeric> struct {
    x: T;
    y: T;

    add::(self: Self, other: Self) -> Self {
        return Self { x = self.x + other.x, y = self.y + other.y };
    }

    scale::(self: Self, scalar: T) -> Self {
        return Self { x = self.x * scalar, y = self.y * scalar };
    }

    operators {
        + = add;
        * = scale;
    }
}

position += velocity * dt;
```

The first version supports binary `+`, `-`, `*`, `/`, unary `-`, and the matching
compound assignments. Operator mapping is exact and left-directed; there is no
conversion ranking, reverse dispatch, trait system, or backend operator machinery.

### Expressions

Supported expression forms include:

- numeric, boolean, character, `null`, and readonly byte-slice string literals
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
takes_s32(x += 1);  // invalid
return x++;         // invalid
```

Mutation targets must denote assignable storage.

Valid targets include mutable variables and fields/indexes whose base is assignable.

Invalid targets include constants, enum members, and fields or indexes derived from temporary values.

### Definite Assignment

Local variables are not implicitly initialized:

```c
value: s32;
```

A variable may be read only when semantic analysis can prove that it has been initialized on every
reachable incoming path.

A direct whole-variable assignment initializes it:

```c
value: s32;

value = 10;

return value;
```

Parameters and variables declared with initializers begin initialized.

Compound assignment and increment/decrement require prior initialization because they read
the previous value:

```c
value: s32;

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
takes_s32(does_nothing());
return does_nothing();
does_nothing() + 1;
```

### Raw Object Pointers

Coglet provides mutable and readonly raw object pointers:

```c
value: s32 = 10;

pointer: s32* = &value;
view: readonly s32* = pointer;
```

`T*` is a raw pointer that grants mutable access to `T`.
`readonly T*` grants read access but does not permit mutation through that
pointer:

```c
read: s32 = *view;  // valid
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
view: readonly s32* = pointer;
other := cast(readonly s32*, pointer);
```

Readonly access cannot implicitly or explicitly become mutable access.
Dereference, pointer indexing, field access, and address-of preserve the
relevant access permission:

```c
readonly_again: readonly s32* = &*view;
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
pointer: s32* = null;          // valid
view: readonly s32* = null;    // valid
pointer: s32* = 0;             // invalid
```

An explicit `null`-to-pointer cast may provide either concrete pointer type:

```c
mutable_null := cast(s32*, null);
readonly_null := cast(readonly s32*, null);
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
pointer: s32* = get_pointer();
handle := reinterpret(opaque*, pointer);
recovered := reinterpret(s32*, handle);
```

### Arrays

```c
values: s32[3] = [1, 2, 3];
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

### Slices and String Literals

A slice is a non-owning pointer-and-length view. `T[]` permits mutation through
the view; `readonly T[]` permits reads only. Fixed, addressable arrays adapt to
matching slices, and mutable slices may weaken to readonly slices. Slice lengths
use target-sized `usize`.

```c
values: s32[3] = [1, 2, 3];
view: s32[] = values;
readonly_view: readonly s32[] = view;

view[1] = 20;
count := readonly_view.len;
first := readonly_view.data;
```

String literals are ordinary `readonly u8[]` expressions:

```c
text := "hello";
// text.len == 5
```

Their compiler-owned backing storage contains a trailing NUL for C interoperability, but that terminator is excluded from the slice's visible length. A literal may still initialize a fixed `u8[N]` array, where the trailing NUL *is* part of the required array size:

```c
name: u8[6] = "hello";
```

Slices are non-owning and lifetime-unchecked in this first version, and slice indexing does not yet add runtime bounds checks. See `docs/language.md` and `docs/known_shortcomings.md`.

### Structs

```c
Point :: struct {
    x: s32;
    y: s32;
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

Coglet has a multi-file frontend with lexer/parser, semantic analysis, explicit
target facts, deterministic semantic metadata, and a verifier that checks the
frontend-to-IR contract. The semantic layer owns type identity, exact overload and
generic specialization decisions, definite-assignment/resource flow, module/import
visibility, C ABI declarations, and source diagnostics.

Successful programs lower into compiler-owned CogIR. The module is verified and
frozen before frontend state is destroyed, so execution backends consume only
backend-neutral runtime types, CFG/storage operations, source provenance, and the
exact ABI metadata needed at foreign boundaries. Runtime requirements are frozen on
the CogIR module and select the split I/O, math, and memory runtime components.

The host-C backend is the bootstrap executable path and covers every current
`CogIrOp`. The optional LLVM backend consumes the same frozen module, verifies LLVM
IR, supports native object/assembly generation and linking, `-O0` through `-O3`, and
source debug information with `-g`. Its represented C aggregate ABI classifier
currently covers x86-64 SysV and Win64; non-x86-64 aggregate classification remains
future cross-target work.

The standard library currently includes I/O, scalar/transcendental math, typed heap
allocation, growable/fixed/scratch arenas, debug allocation instrumentation,
`Array<T>`, and generation-checked `Pool<T>`. Resource values are move-only with
control-flow-aware ownership state, but Coglet intentionally does not infer borrows
or lifetimes for raw pointers and slices.

## Roadmap

Near-term work is tracked in `docs/roadmap.md` and prioritized debt in
`docs/known_shortcomings.md`. The main directions are explicit cross-target/toolchain
selection and native CI, package/platform standard-library growth, unresolved slice
and allocation contracts such as bounds/reslicing, zero-sized elements, and
recoverable allocation, whole-program
reachability/DCE, and larger-program diagnostics/tooling.

## License

Coglet is licensed under the Apache License, Version 2.0.
