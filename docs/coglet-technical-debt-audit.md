# Coglet technical-debt audit — 2026-08-18

## Status

The eight cleanup boundaries in this audit are complete. This file is retained as
a completion record; `README.md`, `docs/language.md`, `docs/compiler.md`,
`docs/ir.md`, `docs/stdlib.md`, `docs/testing.md`, `docs/known_shortcomings.md`, and
`docs/roadmap.md` describe the current architecture and language contracts.

The audit started from `coglet_cpy.tar(20260818-042400).gz` at `13d7d87`
(`cleanup(generics): consolidate specialization infrastructure`). The tree was clean
at that boundary, with 710/710 Debug and Release LLVM-off tests passing and
warning-clean GCC/Clang builds. LLVM development files were not available in the
audit container.

## Completed cleanup boundaries

1. **Frontend parser/header hygiene — completed**
   - disambiguated statement-level explicit generic calls (`name::<T>(...)`)
   - removed redundant C99 typedef redeclarations
   - restored warning-free Clang builds

2. **Remove dead default-parameter half-feature — completed**
   - removed unused default-expression state from parameter AST nodes
   - rejected the old spelling with one explicit parser diagnostic
   - stopped advertising defaults as supported
   - retained default parameters only as a future design item

3. **Semantic call-resolution consolidation — completed**
   - centralized exact argument checking and concrete call selection used by ordinary
     calls, methods, overloads, operators, and generic specializations
   - preserved exact-match behavior and existing diagnostics

4. **Generic specialization consolidation — completed**
   - unified specialization-key/cache/recursion/name machinery for generic functions
     and generic structs
   - preserved `SemDeclId + structural concrete arguments` specialization identity

5. **Ownership/resource-flow correctness — completed**
   - replaced Boolean ownership initialization with a three-state flow lattice
   - made branch/switch/short-circuit ownership merging path-sensitive
   - enforced resource loop-backedge invariants across body/continue/post paths
   - corrected compile-time-true loop/break exit handling
   - fixed `defer` flow snapshotting
   - kept `deinit` an ordinary method rather than an implicit ownership-state change
   - preserved the intentionally non-borrow-checked pointer/slice model

6. **Slice/layout/memory-model normalization — completed**
   - audited readonly/volatile propagation and pointer/array-to-slice construction
   - made native `cfn` callback values layout-queryable while ordinary Coglet function
     values remain non-layoutable
   - preserved callback ABI metadata through ordinary aggregate storage
   - rejected unsupported `cfn`-containing generic specializations at semantic time
   - clarified arena/scratch/fixed-arena invalidation contracts
   - retained zero-sized element allocation/container semantics as explicit design debt

7. **Runtime/toolchain/backend cleanup — completed**
   - split the runtime into I/O, math, and memory components with a compatibility
     umbrella translation unit
   - froze runtime requirements once in CogIR instead of rescanning symbols in the
     driver
   - fixed discarded-expression/result host-C warnings at the IR/backend boundary
   - structurally covered GNU-style and MSVC-style native toolchain argument paths
   - retained native Windows CI as infrastructure work rather than pretending local
     structural validation is native execution

8. **Final docs/test-suite consolidation — completed**
   - removed obsolete tracked root smoke artifacts and binaries
   - renamed chronology/stage-based LLVM fixtures and labels around stable capabilities
   - rewrote reference docs around frontend -> frozen CogIR -> backend architecture
   - refreshed self-hosting and known-shortcoming descriptions
   - removed stale/dead source comments/macros and fixed parser float-token scratch
     hygiene
   - completed the whole-tree stale-terminology/dead-artifact audit

## Follow-on language normalization

After the technical-debt cleanup, target-sized integer aliases and layout-query
syntax were normalized as one feature boundary:

- `usize` / `isize` resolve to the unsigned/signed fixed-width integer matching the
  target pointer width;
- slice lengths and core allocator/container counts use `usize`;
- `size_of(T)` / `align_of(T)` are dedicated type-query forms returning `usize`;
- ordinary generic syntax remains `name::<T>(...)` / `Type::<T>`;
- the old `size_of::<T>()` / `align_of::<T>()` spelling is rejected with a migration
  diagnostic;
- C `size_t` remains the explicit `c_size` ABI alias rather than being conflated with
  source `usize`.

The post-normalization test matrix reached 729 registered LLVM-off tests in the
development environment, with Debug/Release GCC and Clang Debug passing and warning
checks clean. The project documentation should treat test totals as transient; the
current suite, not this historical number, is authoritative.

## Remaining work

The audit is no longer the planning source of truth. Remaining target/toolchain,
memory/slice, reachability, diagnostics, generic/ownership, text/formatting, and
standard-library work is prioritized in `docs/known_shortcomings.md` and
`docs/roadmap.md`.
