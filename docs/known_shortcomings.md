# Known Shortcomings and Follow-up Work

This file is a living engineering-debt log for limitations discovered while
implementing Coglet. It complements `docs/roadmap.md`: the roadmap describes
planned language/compiler direction, while this file records concrete rough
edges, temporary API compromises, missing validation, and cleanup work that
should not be forgotten.

## Runtime and platform support

- **Windows and AArch64 need continuous native CI.** The runtime/native-toolchain
  code is intentionally architecture-neutral and has Linux/Windows paths, but the
  current development environment only executes Linux x86-64. Add native or
  trustworthy cross/VM CI for Linux AArch64, Windows x86-64, and Windows ARM64.
- **Coglet is still native-host only.** `TargetInfo` can describe synthetic
  frontend targets, but the CLI cannot yet select a target triple, backend data
  layout, SDK/sysroot, or cross linker/toolchain.
- **Runtime dependency detection is declaration-based.** The driver currently
  scans frozen external function declarations for the reserved `coglet_rt_*`
  namespace. Importing a module that declares runtime entry points can therefore
  add the runtime even when a particular program does not call them. A later
  reachability/dead-code pass should derive runtime capabilities from reachable
  calls instead.
- **The runtime is one C source file.** Capability macros keep math-only code and
  libraries out of I/O-only links, but the implementation should be split into
  maintainable runtime components once more services (memory, time, filesystem,
  threads) arrive.

## `std.math`

- **Exact overloads deliberately do not perform conversion ranking.** The first
  overload facility matches only exact concrete parameter lists after normal
  untyped-numeric defaulting. `sqrt(1.0)` therefore selects `f64`, and an expected
  destination such as `result: f32 = sqrt(1.0)` does not cause the `f32` overload
  to win. Overloaded function values, generic overloads, method overloads, and C
  ABI overload sets remain future work.
- **Transcendental results are platform-libm results.** Host-C and LLVM call the
  same Coglet runtime ABI, but the runtime delegates to the host C math library.
  The language does not currently guarantee bit-identical transcendental results
  across libc/CRT implementations or architectures. If deterministic simulation
  requires that guarantee, provide a separately specified deterministic math
  implementation rather than silently changing these functions.
- **Math error policy is inherited from C for now.** Domain/range behavior for
  functions such as `sqrt`, `asin`, `acos`, and `fmod` follows the host C math
  implementation (including NaN/infinity behavior). Coglet has not yet selected
  an explicit floating runtime error/`errno`/exception policy.
- **Generic inference does not use an already-inferred peer argument to
  contextualize independent untyped literals during type-argument collection.**
  This was exposed by an attempted `clamp01<T>` implementation calling
  `clamp(value, 0.0, 1.0)` for `T = f32`; the literals independently defaulted to
  `f64` and conflicted. The stdlib currently avoids the pattern instead of adding
  ad-hoc inference rules.

## Vector math

- **Struct constraints still apply to the whole method set.** The vectors use
  `T: numeric` so integer grid vectors and floating gameplay vectors share one type
  family. Generic-struct method bodies are now checked lazily, which lets
  `length`/`distance`/`normalized` exist and succeed for floating specializations
  while an integer vector remains usable until such a method is called. This is a
  semantic validity mechanism, not an explicit method contract; method-specific
  constraints should still be designed with future generic-method constraints.
- **Vector normalization has a non-zero precondition.** `normalized()` divides by
  `length()` and currently has no `try_normalized`, epsilon policy, or zero-vector
  fallback. Game code that may normalize degenerate vectors must guard that case.
- **Vector arithmetic is scalar arithmetic, not SIMD or special backend IR.** The
  current vectors are ordinary Coglet structs. They have no guaranteed packed/SIMD
  ABI, no shader-vector ABI promise, and no automatic vector instruction mapping.
  Optimize only after profiling and after a target/ABI policy is designed.
- **Vector operators are intentionally narrow.** `+`/`-` map to vector add/sub,
  `*`/`/` map to scalar multiply/divide, and unary `-` maps to component negation.
  Component-wise multiplication remains `.mul(other)`, reverse scalar multiplication
  (`scalar * vector`) is not synthesized, and swizzles such as `.xy`/`.xyz` remain
  deferred rather than compiler magic.
- **Unsigned vector subtraction/cross products retain ordinary checked scalar
  semantics.** `numeric` includes unsigned integers, so operations that mathematically
  require a negative component can trigger the same checked underflow behavior as
  equivalent scalar Coglet code. A future richer constraint vocabulary may allow
  APIs to distinguish signed-or-floating vector operations without inventing one-off
  vector rules.

## Matrix and quaternion math

- **Matrices and quaternions are ordinary scalar structs, not SIMD or graphics-ABI types.**
  `Mat3<T>`, `Mat4<T>`, and `Quat<T>` intentionally reuse generic structs and
  methods. There is no guaranteed packed/SIMD layout, shader ABI, vector-register
  calling convention, or automatic backend vectorization contract.
- **The matrix convention is fixed but projection conventions are not yet exposed.**
  Matrices use column-vector transforms with `A * B` applying `B` first. The
  library currently omits perspective/orthographic/look-at helpers rather than
  silently selecting handedness and clip-depth conventions. Future helpers should
  use explicit convention-bearing names where the distinction matters.
- **`Mat4` is currently affine-transform oriented.** `transform_point` includes
  translation but does not perform a homogeneous perspective divide. Add a
  separate projective point operation when projection matrices are introduced.
- **General matrix inverses are not implemented yet.** `Mat3` exposes a
  determinant, but neither `Mat3` nor `Mat4` currently exposes general `inverse`
  or singularity/error handling. Rigid/affine specialized inverses may be more
  useful for games and should be designed alongside the error policy.
- **Quaternion constructors and interpolation have explicit validity preconditions.**
  `from_axis_angle` requires a non-zero axis, `rotate_vector` assumes a normalized
  quaternion for a pure rotation, `inverse` requires a non-zero quaternion, and
  `slerp` expects normalized inputs. There are no checked/try variants or epsilon
  policy yet.
- **Euler-angle conversion is intentionally absent.** Rotation order and angle
  convention are easy to make ambiguous. Add explicitly named Euler helpers only
  after choosing and documenting their order semantics.

## Compiler/build hygiene

- **Clang currently reports repeated typedef-redefinition warnings in existing
  frontend headers.** A Clang build succeeds and the stdlib runtime tests pass,
  but `Parser`, `Type`, and `Symbol` forward/full typedef declarations trigger
  `-Wtypedef-redefinition`. This predates the runtime-math work and should be
  cleaned up so the project's warning-free policy holds across both GCC and
  Clang, not only the normal GCC configuration.

## Code generation and linking

- **No whole-program reachability/DCE yet.** Importing a module can emit unused
  concrete internal functions in host-C output. Generated C marks such functions
  as intentionally maybe-unused to keep normal builds warning-free; a later
  reachability pass should avoid emitting them in the first place.
- **Arbitrary non-C external symbol spellings remain GNU/Clang-only in host-C.**
  Identifier-safe external C symbols are portable and work with MSVC; arbitrary
  linker names still require the GNU `__asm__("symbol")` source extension.
- **Raw linker/SDK control is limited.** The driver supports `-L` and `-l`, but
  not general raw linker flags, sysroots, framework-style inputs, or a first-class
  platform SDK model.

## Strings and I/O

- **Slices are non-owning and have no lifetime/escape analysis.** `T[]` / `readonly T[]` prevent immediate array-temporary conversion, but the compiler does not yet prove that a slice cannot outlive local backing storage. String-literal slices are safe because their backing storage is compiler-owned static data.
- **Slice indexing is currently unchecked at runtime.** Constant fixed-array indexes retain compile-time bounds checks, but a slice index does not yet trap/check `index < len`. Decide the language's checked/unchecked indexing policy before slices become a safety boundary.
- **Slice length is fixed `u64`, not a target-sized `usize`.** This matches the current 64-bit Linux/Windows x86-64/AArch64 target focus but should be revisited before 32-bit targets or a general target-sized integer type are supported.
- **Slice syntax cannot independently qualify an enclosing slice when its element is itself a qualified pointer.** Prefix `readonly` retains the existing first-pointer-layer binding. A future qualifier grammar may need a clearer nested-type spelling.
- **There is no reslicing/subview syntax yet.** `.data`, `.len`, indexing, array-to-slice conversion, and mutable-to-readonly weakening exist, but operations such as `view[a:b]` are deferred.
- **There is no distinct first-class text/string type yet.** `readonly u8[]` is the general byte-string view. Encoding/UTF-8 policy, owned strings, and text-specific APIs remain separate design work.
- **Formatting is intentionally primitive.** Users currently compose text with
  explicit scalar printer calls. There is no formatting language, interpolation,
  generic formatting protocol, or variadic type-safe `print`.
- **I/O error reporting is not surfaced.** The v0 print/flush routines return
  `void`; write failures and stream errors are not represented in Coglet yet.

## Generics and aggregate types

- **Generic structs are intentionally restricted.** Ordinary top-level Coglet
  structs may be generic, but generic enums, unions, aliases, nested generic
  declarations, and generic `#repr(c)` aggregates are not implemented.
- **Generic struct type arguments are explicit.** The compiler does not infer
  `Vec2::<f32>` from an initializer or expected destination type, and there are no
  default/partial type arguments. This keeps nominal construction predictable but
  may become verbose for container-heavy code.
- **Recursive generic specialization uses a compiler termination guard.** Finite
  pointer recursion is supported and by-value recursive layouts are rejected, but
  a chain that continually changes concrete type arguments is currently stopped at
  32 active instantiations rather than by a more general well-foundedness proof.
- **Methods are intentionally narrow.** Methods/associated functions are supported
  only on ordinary complete Coglet structs. There are no methods on `#repr(c)`
  aggregates, unions, incomplete structs, or foreign/extension types.
- **Generic methods are not implemented.** A generic struct may have methods, but a
  method cannot currently introduce its own `::<U>` type-parameter list.
- **Method visibility follows the owning struct.** There is no independent
  per-method private/export control yet, and there is no method overloading.
- **Method values are not first-class.** `value.method()` and `Type.function()` are
  calls, but extracting `value.method` or `Type.function` as a callable value is not
  supported.
- **Pointer receivers still use explicit dereference in method bodies.** The caller
  can write `value.set_x(1)`, but the body currently writes `(*self).x`; Coglet has
  no implicit pointer-field dereference or `->` syntax.
- **Generic struct constraints apply to the whole attached method set.** Concrete
  specialization resolves every method signature, but generic-struct method bodies
  are checked lazily on first use. That permits specialization-specific body
  validity without declaring the requirement in the method signature. There is
  still no explicit method-specific generic constraint mechanism.
- **User-defined operators are deliberately restricted.** Only binary `+`, `-`, `*`,
  `/`, unary `-`, and matching arithmetic compound assignments are supported. A
  mapping names one instance method with a by-value `Self` receiver and `Self` return;
  there are no operator overload sets, conversion ranking, reverse/symmetric dispatch,
  comparison/equality operators, indexing/call operators, custom precedence, or
  logical/bitwise operator hooks. Expand this only when concrete stdlib/game APIs
  demonstrate a need.
- **Builtin generic constraints are closed.** The current constraint set is a
  small compiler-defined vocabulary, not a user-extensible trait/interface
  system. Do not grow it into an accidental trait system one special case at a
  time.

## Documentation/process

- Keep build/install/runtime examples current for both host-C and LLVM as the
  runtime grows.
- Every claimed platform should eventually have a CI job that builds the
  compiler, runs the complete suite, and executes at least `std.io` and
  runtime-backed `std.math` through both available backends.
- Add new concrete shortcomings here when a milestone exposes them, even when a
  local workaround is sufficient for the current patch.
