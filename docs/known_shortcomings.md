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

- **Runtime math has precision-suffixed public names.** Coglet has no overload
  resolution or compile-time type dispatch, so the first runtime API exposes
  pairs such as `sin_f32`/`sin_f64` and `sqrt_f32`/`sqrt_f64`. A future language
  mechanism should allow the public spelling `std.math.sin(x)` without adding a
  special compiler rule solely for the standard library.
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

- **Generic structs/enums are not implemented.** Generic functions are
  monomorphized before CogIR, but reusable aggregate containers such as
  `Vec3<T>`, owned/growable containers, `Optional<T>`, and `Result<T, E>` still need a coherent
  aggregate-instantiation design.
- **No methods/associated functions or operator overloading.** These should be
  designed after generic aggregates so vector/matrix code can drive the language
  design rather than adding special compiler magic for game-math types.
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
