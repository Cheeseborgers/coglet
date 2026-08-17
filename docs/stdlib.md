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

`std.math` is a portable scalar-math foundation aimed at ordinary application
and game code. Pure-Coglet helpers and constants share the module with a first
runtime-backed transcendental/rounding slice; both host-C and LLVM call the same
reserved runtime ABI for those operations:

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

Vec2<T: numeric>
Vec3<T: numeric>
Vec4<T: numeric>

sqrt_f32/sqrt_f64
sin_f32/sin_f64
cos_f32/cos_f64
tan_f32/tan_f64
asin_f32/asin_f64
acos_f32/acos_f64
atan_f32/atan_f64
atan2_f32/atan2_f64
floor_f32/floor_f64
ceil_f32/ceil_f64
round_f32/round_f64
trunc_f32/trunc_f64
fmod_f32/fmod_f64
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

### Numeric vectors

`std.math` exports `Vec2<T>`, `Vec3<T>`, and `Vec4<T>` as ordinary generic
Coglet structs constrained to `numeric`. They are not compiler intrinsics and do
not have backend-specific or SIMD semantics. The scalar type therefore controls
all arithmetic, overflow, precision, and conversion behavior.

```c
position := std.math.Vec3::<f32>.new(10.0, 5.0, 2.0);
velocity := std.math.Vec3::<f32>.new(1.0, 0.0, -2.0);
next := position.add(velocity).scale(0.5);

tile := std.math.Vec2::<s32>.new(12, 7);
```

Each vector provides `new`, `zero`, `one`, and `splat` constructors plus these
common operations:

```text
add(other)              component addition
sub(other)              component subtraction
mul(other)              component-wise multiplication
scale(scalar)           scalar multiplication
dot(other)              dot product
length_squared()         squared Euclidean length
distance_squared(other) squared Euclidean distance
min(other)              component-wise minimum
max(other)              component-wise maximum
clamp(low, high)         component-wise clamp
```

`Vec3<T>` additionally provides `cross(other)`. Because the vectors admit every
`numeric` type, integer and unsigned arithmetic follows the normal Coglet scalar
rules; for example, an unsigned subtraction or cross-product component can still
hit the language's ordinary checked underflow behavior.

The first vector slice intentionally exposes squared length/distance rather than
`length`, `distance`, or normalization. Those require selecting the concrete
`f32` or `f64` runtime square-root entry point from generic code, while the
current generic system has neither overload resolution nor compile-time type
dispatch and struct constraints currently apply to the entire attached method
set. The stdlib does not hide that gap behind lossy casts or approximation code.
Likewise, operator overloading is not yet present; the method spelling is the
stable explicit API until that language design is addressed.

The runtime-backed functions use precision-explicit names because Coglet does not
yet have overload resolution or compile-time type dispatch. For example,
`sin_f32` and `sin_f64` map to distinct reserved runtime ABI symbols but remain
ordinary `#extern(c)` declarations from the language's point of view. This is a
temporary public API compromise tracked in `docs/known_shortcomings.md`; it does
not justify a standard-library-only compiler special case.

The transcendental functions delegate to the host C math implementation through
`stdlib/runtime/coglet_runtime.c`. `floor`, `ceil`, `trunc`, and `fmod` follow the
corresponding C semantics; `round` rounds halfway cases away from zero. Domain and
range behavior, NaN/infinity propagation, and last-bit transcendental results are
therefore currently the host C library's behavior. Tests use tolerances for
transcendental results rather than requiring cross-platform bit identity.

On GNU/Clang-style Linux links the native-toolchain layer enables the runtime math
capability and adds `libm`; Windows uses the normal C runtime math implementation.
The same capability flag is used when linking an LLVM-emitted native object, so
neither backend implements math functions itself.

## `std.io`

`std.io` is the first I/O-facing runtime-backed standard module. The public API remains
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
print(readonly u8[])
println(readonly u8[])
newline()
flush()
print_bool(bool)
print_s8/s16/s32/s64
print_u8/u16/u32/u64
print_f32/f64
```

`print` and `println` accept readonly byte slices. String literals infer directly to that type, and arbitrary byte views may be printed without requiring NUL termination. The Coglet wrapper passes `.data` and `.len` to the private `coglet_rt_io_write` runtime ABI, so slices themselves do not cross the C ABI by value. Formatting remains later library/language work. The scalar printers are explicit rather than variadic so the first I/O API does not require variadic generics or runtime type descriptors.
`print_f32` uses enough significant decimal digits to round-trip a binary32 value;
`print_f64` does the corresponding binary64 formatting.

Runtime implementation symbols use the reserved `coglet_rt_` prefix. After
frontend state is destroyed, the driver scans frozen CogIR C-ABI symbol metadata;
a compilation unit containing those declarations causes
`<stdlib-root>/runtime/coglet_runtime.c` to be compiled/linked. Math symbols use
the narrower `coglet_rt_math_` prefix to enable the runtime's math capability and
its platform-specific native math-library linkage. CogIR contains no
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
also compares exact stdout while exercising every v0 scalar printer. The math
consumer exercises both `f32` and `f64` runtime functions with tolerance-based
checks for transcendental results and exact checks for rounding/remainder cases. Run the
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
