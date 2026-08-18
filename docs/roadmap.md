# Coglet Roadmap

This document is future-facing. Completed implementation history belongs in Git;
concrete unresolved limitations belong in `docs/known_shortcomings.md`.

## Current architecture

Coglet has a multi-file frontend with lexer/parser, semantic analysis, explicit
frontend target facts, deterministic semantic metadata, and a semantic-information
verifier. Named modules are private by default, imports can be discovered from
importer-relative paths, repeated `-I` roots, and the configured `std.*` root, and
successful compilation produces one frozen CogIR module.

CogIR is the frontend/backend boundary. It owns backend-neutral runtime types,
constants, globals, functions, CFG/storage operations, source provenance, executable
entry identity, exact ABI metadata where foreign storage/calls require it, and the
frozen runtime-component requirement. Frontend state is destroyed before execution
backends run.

The host-C backend is the bootstrap execution path and covers every current CogIR
operation. The optional LLVM backend consumes the same frozen module and supports
verified LLVM IR, native object/assembly generation and linking, `-O0` through
`-O3`, and `-g`. The LLVM represented-aggregate classifier currently covers x86-64
SysV and Win64.

The runtime is split into I/O, math, and memory components selected from frozen
CogIR requirements. The standard library includes I/O, math/vector/matrix/quaternion
facilities, typed heap allocation, arenas/scratch/fixed arenas, debug allocation,
`Array<T>`, and generation-checked `Pool<T>`.

The language includes exact overloads, restricted generic functions/structs,
methods/operators, slices, raw pointers, target-sized `usize`/`isize` integer aliases,
`size_of(T)` / `align_of(T)` target-layout queries, must-use function results with
explicit `discard` and declaration-level `#discardable`, `defer`, move-only
`resource` values, and a substantial explicit C interop surface. Slice lengths and
core container/allocator counts use `usize`. Ownership flow is control-flow-aware
for direct local/parameter owners; raw pointers and slices deliberately remain
outside a borrow/lifetime checker.

## Priority 1: target, toolchain, and CI

The next infrastructure priority is to make the existing target boundaries usable
outside the build host:

- add explicit CLI target selection and a target-triple/C-ABI preset model;
- define sysroot/SDK/linker selection rather than relying on the host toolchain;
- extend represented C aggregate classification beyond x86-64 SysV/Win64 when a
  supported target requires it;
- continuously run the compiler/runtime/backend suite on Linux AArch64, Windows
  x86-64, and Windows ARM64 in addition to the current Linux x86-64 development
  environment;
- keep host-C and LLVM behavior aligned through shared CogIR/runtime contracts.

Cross-target work should extend `TargetInfo` only for frontend-visible semantic
facts. LLVM `DataLayout`, relocation, register classes, and other backend details
remain backend-owned.

## Priority 2: package and platform library boundary

The module system is sufficient for source discovery but is not yet a package
manager/build graph. The next library/tooling layer should add:

- package manifests and automatic package membership for multi-file libraries;
- file I/O, clocks/time, filesystem/path services, and explicit platform services;
- a stable standard-library/runtime ABI/versioning policy for installed packages;
- first-class dependency/link metadata where a package needs native libraries;
- APIs suitable for compiler/self-hosting workloads rather than only small examples.

Threads, sockets, process APIs, and virtual-memory primitives should follow the same
runtime/platform boundary instead of becoming backend-specific intrinsics.

## Priority 3: memory and slice contracts

Several intentionally deferred policies should be resolved before slices/containers
are treated as a stronger safety boundary:

- choose checked versus unchecked runtime slice indexing and specify failure
  behavior;
- define or reject zero-sized element allocation/container behavior;
- add recoverable allocation APIs alongside the current infallible allocation path;
- decide whether resource field moves, resource globals, or resource-valued
  containers are needed and define their destruction/transfer rules before adding
  them;
- add reslicing/subviews and richer nested qualifier syntax only after the lifetime
  and bounds contracts are clear.

Coglet is not currently planning an implicit Rust-style borrow checker. If stronger
lifetime guarantees are added, they should preserve the language's explicit systems
model and be justified by concrete APIs that cannot be made safe enough otherwise.

## Priority 4: reachability and compilation scaling

The compiler currently lowers one frozen whole-program CogIR unit. Scaling work
should preserve the existing semantic/backend boundary while reducing unnecessary
work:

- add whole-program reachability/DCE so unused internal functions and unused runtime
  components are not emitted merely because declarations are present;
- derive runtime requirements from reachable calls once such a pass exists;
- define stable declaration/module identities suitable for caching or separate
  compilation;
- make package/module dependency information explicit without turning source import
  discovery into runtime initialization ordering;
- profile specialization/lowering memory use before introducing more complex caching.

## Priority 5: diagnostics and tooling

The frontend already preserves source provenance through CogIR. Larger programs need
better presentation and tooling around that data:

- diagnostic notes/secondary spans and richer parser/semantic recovery;
- machine-readable diagnostics for editor/build integration;
- lexical debug scopes in CogIR if debugger behavior demonstrates a need beyond the
  current function-level local scope;
- source-location facilities that can also improve runtime/debug-allocation reports;
- incremental tooling APIs only after stable package/module identity exists.

## Language design queue

These are design items, not implied near-term commitments:

### Generics and overloads

- method-specific generic parameters/constraints;
- generic enums/aliases or user-defined constraints if real library APIs justify
  them;
- an explicit ABI-provenance model before `cfn`-containing generic arguments are
  accepted;
- conversion-ranked overloads only if exact matching proves insufficient.

### Ownership and cleanup

- resource field moves and resource globals;
- explicit destruction/drop semantics if manual `defer value.deinit()` becomes too
  error-prone;
- ownership-aware containers only with defined element move/destruction behavior.

### Slices and text

- bounds policy and reslicing/subviews;
- a first-class string/text type only after encoding, ownership, mutation, and C
  interop are specified.

### C interoperability

The current interop surface already covers native scalar aliases, symbol overrides,
raw/incomplete/volatile pointers, callbacks/function pointers, variadics, calling
conventions, represented structs/unions/enums/arrays, packed/explicit alignment, and
x86-64 aggregate ABIs. Further work should be target/toolchain driven rather than
adding syntax speculatively.

## Backend direction

Host-C remains the bootstrap/reference execution path for frozen CogIR. LLVM owns
machine optimization, native object/assembly emission, native debug metadata, and
backend-specific ABI classification. A custom native backend or interpreter remains
possible, but should consume the same frozen CogIR contract rather than retaining
frontend state or redefining language semantics.

## Self-hosting direction

Self-hosting is a long-term validation target rather than the next compiler feature.
The main blockers are practical library/toolchain capabilities: file I/O, richer
platform services, package/build metadata, robust diagnostics, cross-target policy,
and enough compiler-facing standard library to implement the compiler without a
large C support layer. See `docs/selfhosting.md` for the current requirement list.
