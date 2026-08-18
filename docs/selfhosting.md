# Self-Hosting Requirements

Self-hosting remains a long-term validation objective rather than a near-term
language milestone. The compiler architecture is already suitable for it: semantic
analysis lowers into frozen CogIR, and both host-C and LLVM consume that IR without
retaining frontend objects.

## Capabilities already present

The language/frontend already provides the core facilities a compiler implementation
would expect:

- structs, enums, fixed arrays, slices, raw pointers, function values, and explicit C
  interoperability;
- exact overloads, restricted generic functions/structs, methods, and restricted
  user-defined arithmetic operators;
- structured control flow, `defer`, definite assignment, unified reachability, and
  move-only resource flow for direct local/parameter owners;
- compile-time constants, checked/wrapping numeric semantics, casts, target-sized
  `usize`/`isize` aliases, target-layout queries, and deterministic semantic metadata;
- multi-file named modules/imports with private-by-default visibility and source
  discovery through importer-relative paths, `-I`, and the configured `std.*` root;
- a frozen compiler-owned IR with verifier coverage and source provenance;
- host-C execution plus optional LLVM native object/assembly/link, optimization, and
  debug-information paths;
- standard-library I/O, math, heap allocation, arenas/scratch/fixed arenas, debug
  allocation, growable arrays, and fixed-capacity pools.

## Practical blockers

The remaining blockers are mostly library, package, target, and tooling work rather
than another fundamental semantic-analysis subsystem:

- file I/O, path/filesystem APIs, clocks/time, and other platform services needed by
  a compiler driver;
- package manifests and a stable compilation-unit/library/dependency model beyond
  source import discovery;
- explicit target/cross-toolchain selection, SDK/sysroot/linker policy, and native CI
  across the claimed host matrix;
- stronger diagnostics for large projects, including secondary spans/recovery and a
  machine-readable interface;
- a stable installed standard-library/runtime ABI/versioning boundary;
- whole-program reachability/DCE and eventually compilation caching/separate
  compilation if compiler scale justifies them;
- additional containers/text utilities as demanded by an actual Coglet compiler
  port, rather than speculative language features.

## Deferred language questions

These may become useful during a self-hosting attempt but are not prerequisites by
definition:

- closure environments/captures for nested runtime functions;
- first-class owned strings/text and an encoding policy;
- richer generic constraints or generic methods;
- resource field moves/resource globals/ownership-aware containers;
- interfaces/traits or broader operator hooks;
- stronger compile-time execution/reflection.

A self-hosting effort should first implement a representative compiler subsystem in
Coglet and use the resulting friction to prioritize these items. It should not add
features merely to mirror the implementation language of the bootstrap compiler.
