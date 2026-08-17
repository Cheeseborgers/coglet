# Coglet Standard Library

Coglet ships standard-library modules as ordinary Coglet source beneath the
compiler's configured standard-library root. The source tree mirrors the
installed layout:

```text
stdlib/
├── std/
│   ├── io.cog
│   └── math.cog
└── runtime/
    └── coglet_runtime.c
```

A normal installation places both trees beneath `COGLET_STDLIB_INSTALL_DIR`, so
`stdlib/std/math.cog` becomes `<stdlib-root>/std/math.cog` and is discovered by:

```c
import std.math;
```

Standard-library modules use the same parser, semantic analysis, CogIR lowering,
and backend paths as user modules. There is no privileged backend implementation
or hidden intrinsic module lookup after source discovery.

## `std.math`

`std.math` is the first shipped module. Its initial API is a portable scalar-math
foundation aimed at ordinary application and game code while Coglet does not yet
have a dedicated platform math-runtime/intrinsic boundary:

```c
import std.math;
```

Exports:

```text
pi          : adaptable floating compile-time constant
tau         : adaptable floating compile-time constant
e           : adaptable floating compile-time constant
half_pi     : adaptable floating compile-time constant
quarter_pi  : adaptable floating compile-time constant
inv_pi      : adaptable floating compile-time constant
inv_tau     : adaptable floating compile-time constant
sqrt_2      : adaptable floating compile-time constant
deg_to_rad  : adaptable floating compile-time constant
rad_to_deg  : adaptable floating compile-time constant

abs_s32                       : (s32) -> s32
min<T: ordered>               : (T, T) -> T
max<T: ordered>               : (T, T) -> T
clamp<T: ordered>             : (T, T, T) -> T
clamp01<T: floating>          : (T) -> T
lerp<T: floating>             : (T, T, T) -> T
inverse_lerp<T: floating>     : (T, T, T) -> T
remap<T: floating>            : (T, T, T, T, T) -> T
smoothstep<T: floating>       : (T, T, T) -> T
to_radians<T: floating>       : (T) -> T
to_degrees<T: floating>       : (T) -> T
gcd_u64                       : (u64, u64) -> u64
```

The floating constants are written as hexadecimal floating-point source constants
and remain adaptable `untyped-float` compile-time values rather than being
permanently materialized as `f64`. Normal context selects the concrete precision:

```c
angle32: f32 = std.math.pi;
angle64: f64 = std.math.pi;
defaulted := std.math.pi; // inferred mutable storage defaults to f64
```

No cast is required at the use site. The same source constant is rounded to the
requested `f32` or `f64` representation by ordinary constant materialization.
`deg_to_rad` and `rad_to_deg` provide the same adaptable factors used by
`to_radians` and `to_degrees`.

`min`, `max`, and `clamp` use the `ordered` generic constraint. `clamp(value,
low, high)` requires `low <= high`; it returns the nearest endpoint when `value`
is outside that interval. `clamp01` is the floating-point convenience form for
`[0, 1]`.

`lerp(a, b, t)` performs linear interpolation as `a + (b - a) * t`; `t` is not
clamped, so values outside `[0, 1]` intentionally extrapolate. `inverse_lerp`
requires distinct endpoints. `remap` combines inverse interpolation and
interpolation and likewise requires a non-zero input interval. `smoothstep`
performs the common cubic Hermite interpolation after clamping to `[0, 1]` and
requires `edge0 < edge1`. These functions are currently constrained to
`floating` because Coglet has not introduced a broader algebraic constraint
system.

`abs_s32` uses ordinary checked Coglet negation, so applying it to the minimum
`s32` value follows the language's normal checked-overflow trap semantics.
`gcd_u64` uses the Euclidean algorithm and accepts zero operands; `gcd_u64(0, n)`
and `gcd_u64(n, 0)` return `n`.

The module intentionally does not yet implement transcendental/platform math
operations such as `sin`, `cos`, `sqrt`, `atan2`, or floating remainder. Those
should be added only after Coglet has a deliberate portable math-runtime or
intrinsic boundary shared by host-C and LLVM, rather than by embedding backend-
specific behavior in `std.math`.

## `std.io`

`std.io` is the first runtime-backed standard module. The public API remains
ordinary Coglet source in `stdlib/std/io.cog`; it declares a small reserved C ABI
implemented by `stdlib/runtime/coglet_runtime.c`. User code imports only the
module:

```c
import std.io;

main::() -> s32 {
    std.io.println("score:");
    std.io.print_s32(42);
    std.io.newline();
    std.io.flush();
    return 0;
}
```

Initial exports are:

```text
print(readonly c_char*)
println(readonly c_char*)
newline()
flush()
print_bool(bool)
print_s8/s16/s32/s64
print_u8/u16/u32/u64
print_f32/f64
```

`print` and `println` intentionally accept the current direct C-string boundary,
not a general Coglet string value. String/slice types and formatting remain later
language/library work. The scalar printers are explicit rather than variadic so
the first I/O API does not require variadic generics or runtime type descriptors.
`print_f32` uses enough significant decimal digits to round-trip a binary32 value;
`print_f64` does the corresponding binary64 formatting.

The implementation symbols use the reserved `coglet_rt_` prefix. After frontend
state is destroyed, the driver scans frozen CogIR C-ABI symbol metadata; only a
module that actually references that prefix causes
`<stdlib-root>/runtime/coglet_runtime.c` to be compiled/linked. CogIR contains no
`std.io` or runtime-module special case, and both host-C and LLVM executables link
the same runtime source through the shared native-toolchain layer. `--emit-c`
continues to emit the translation unit without embedding the runtime implementation.

The v0 runtime ABI deliberately uses ISO C scalar/pointer types and stdio. The
native-host support target is Linux and Windows on x86-64 and AArch64. Coglet does
not yet expose a cross-target compiler/toolchain CLI: the runtime and executable
link step use the platform/architecture selected by the CMake toolchain that built
Coglet. Windows builds use the configured MSVC-style or GNU/Clang-style C compiler;
Linux builds use the configured GNU/Clang-style driver. Platform-specific OS APIs
should remain behind this runtime boundary as the library grows.

## Source-tree testing

The shipped integration consumers live at:

```text
tests/stdlib/math/main.cog
tests/stdlib/io/main.cog
```

They import the modules through normal standard-library discovery. The I/O test
also compares exact stdout while exercising every v0 scalar printer. Run the
shipped stdlib slice
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
7. be installable beneath the configured stdlib root;
8. if it needs OS/libc services, bind only the reserved runtime ABI rather than
   embedding backend/platform behavior in the source module.

Runtime implementations live under `stdlib/runtime/` and must preserve the frozen
CogIR/backend boundary. New runtime families should be added deliberately rather
than letting standard modules call unrelated host APIs ad hoc.
