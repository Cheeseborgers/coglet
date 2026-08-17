# Coglet Standard Library

Coglet ships standard-library modules as ordinary Coglet source beneath the
compiler's configured standard-library root. The source tree mirrors the
installed layout:

```text
stdlib/
└── std/
    └── math.cog
```

A normal installation places that tree beneath `COGLET_STDLIB_INSTALL_DIR`, so
`stdlib/std/math.cog` becomes `<stdlib-root>/std/math.cog` and is discovered by:

```c
import std.math;
```

Standard-library modules use the same parser, semantic analysis, CogIR lowering,
and backend paths as user modules. There is no privileged backend implementation
or hidden intrinsic module lookup after source discovery.

## `std.math`

`std.math` is the first shipped module. Its API remains deliberately small; the
first generic-function milestone uses `min` and `max` as the standard-library
proof that one source definition can produce concrete specializations:

```c
import std.math;
```

Exports:

```text
pi       : f64 constant
tau      : f64 constant
abs_i32  : (i32) -> i32
min<T>   : (T, T) -> T
max<T>   : (T, T) -> T
gcd_u64  : (u64, u64) -> u64
```

`abs_i32` uses ordinary checked Coglet negation, so applying it to the minimum
`i32` value follows the language's normal checked-overflow trap semantics.
`gcd_u64` uses the Euclidean algorithm and accepts zero operands; `gcd_u64(0, n)`
and `gcd_u64(n, 0)` return `n`.

`min` and `max` infer `T` from ordinary arguments when inference is
unambiguous, and callers may spell an explicit specialization such as
`std.math.min::<u64>(30, 20)`. Their comparison operators are not assumed valid
for every `T`: each concrete specialization is checked by the ordinary semantic
rules, so an unsupported type is rejected at instantiation. `abs_i32` and
`gcd_u64` remain intentionally concrete; this milestone does not attempt to make
every standard-library function generic.

## Source-tree testing

The integration program for the module lives at:

```text
tests/stdlib/math/main.cog
```

It imports `std.math` through the normal standard-library discovery mechanism and
checks the public API as an ordinary user program. Run the shipped stdlib slice
with:

```sh
cmake --build <build-dir> --target check_stdlib
```

or directly through CTest:

```sh
ctest --test-dir <build-dir> -L stdlib.source --output-on-failure
```

LLVM-enabled builds run the same program through host-C plus LLVM `-O0` and
`-O3`; LLVM-disabled builds retain the host-C integration test.

To compile the test application manually from the source checkout:

```sh
<build-dir>/coglet tests/stdlib/math/main.cog \
    --stdlib-root ./stdlib \
    -o <build-dir>/test-artifacts/manual_std_math
```

For LLVM builds:

```sh
<build-dir>/coglet tests/stdlib/math/main.cog \
    --stdlib-root ./stdlib \
    --backend llvm -O3 \
    -o <build-dir>/test-artifacts/manual_std_math_llvm
```

## Build-tree test artifacts

CTest-generated executables, emitted LLVM IR/assembly, copied test assets, and
similar runtime products live under:

```text
<build-dir>/test-artifacts/
```

rather than being written into the build root. Test/developer helper executables
are placed under `test-artifacts/tools` and the test-only linker support library
under `test-artifacts/lib`. CMake's own `Testing/` metadata directory remains
controlled by CTest. Normal compiler build products such as `coglet`,
`compiler_core`, and backend libraries remain normal build outputs.

## Adding a standard module

A new shipped standard module should:

1. live beneath `stdlib/std/` using the module-to-path mapping (`std.foo` ->
   `stdlib/std/foo.cog`);
2. declare the matching absolute module name;
3. export only its intended public API;
4. use ordinary Coglet semantics rather than backend-specific behavior;
5. have an integration consumer under `tests/stdlib/`;
6. run through host-C and LLVM where the LLVM backend is enabled;
7. be installable with the existing `stdlib/std` installation rule.

Platform/runtime-facing modules such as I/O and allocation should wait for an
explicit runtime boundary rather than embedding host assumptions in otherwise
portable standard modules.
