# Coglet technical-debt audit — 2026-08-18

## Authoritative baseline

Source: `coglet_cpy.tar(20260818-031945).gz`

- HEAD: `cc346e5 feat(memory): add debug allocator instrumentation`
- Debug / LLVM OFF: 709/709 passed, zero GCC configure/build warnings
- Release / LLVM OFF: 709/709 passed, zero GCC configure/build warnings
- LLVM development configuration is not present in the audit container.
- Clang 17 baseline built successfully but reported repeated `Parser`, `Type`, and `Symbol` typedef-redefinition warnings.

The uploaded working tree was not clean before cleanup work began. These tracked root-level smoke artifacts were already absent:

- `mem_temp.cog`
- `struct_test.cog`
- `struct_test_llvm`
- `test_type_directed_math.cog`
- `test_vectors.cog`
- `test_vectors_llvm`

They are treated as pre-existing state and are not included in the cleanup patches below.

The source contains `resource`/`move`, `FixedArena`, `Scratch`, `Pool<T>`, `copyable`, and `DebugAllocator`. Several of those changes were bundled into commit `cc346e5` rather than appearing as the separate milestone commits discussed during development; this is history hygiene, not missing source functionality.

## Cleanup patch boundaries

1. **Frontend parser/header hygiene — completed**
   - disambiguate statement-level explicit generic calls (`name::<T>(...)`)
   - remove redundant C99 typedef redeclarations
   - restore warning-free Clang builds

2. **Remove dead default-parameter half-feature — completed**
   - remove unused default-expression state from parameter AST nodes
   - reject the old spelling with one explicit parser diagnostic
   - stop advertising defaults as supported
   - retain default parameters as a future design item only

3. **Semantic call-resolution consolidation**
   - centralize exact argument checking and concrete call selection used by ordinary calls, methods, overloads, operators, and generic specializations
   - preserve current diagnostics and exact-match behavior
   - do not add conversion ranking or new overload features

4. **Generic specialization consolidation**
   - unify common specialization-key/cache/recursion/name machinery for generic functions and generic structs
   - audit lazy generic-method checking and synthetic semantic-info handling
   - preserve `SemDeclId + structural concrete arguments` identity

5. **Ownership/resource-flow correctness audit**
   - systematically test assignment, parameters, returns, branches, loops, `defer`, moves, resource containment, and use-after-move
   - preserve the intentionally non-borrow-checked pointer/slice model
   - fix only concrete ownership-flow correctness gaps

6. **Slice/layout/memory-model normalization**
   - audit readonly/volatile propagation, pointer-to-slice construction, target layout queries, allocator ABI metadata, and invalidation contracts
   - keep current `u64` lengths during cleanup unless a correctness issue forces otherwise
   - defer `usize`, bounds-check policy, and reslicing as explicit features

7. **Runtime/toolchain/backend cleanup**
   - split the monolithic runtime implementation into coherent components
   - replace scattered reserved-symbol capability scans with a clearer frozen runtime requirement if possible without frontend leakage
   - fix host-C discarded-result temporary warnings
   - audit host-C / LLVM metadata and ABI consistency
   - validate Windows/Linux toolchain paths structurally; native CI remains a separate infrastructure task

8. **Final docs/test-suite consolidation — last**
   - rewrite docs around the cleaned architecture rather than patch chronology
   - remove stale root smoke artifacts if their deletion is intentional
   - consolidate CTest labels/fixtures and obsolete examples
   - prioritize remaining `known_shortcomings.md` entries
   - run final whole-tree warning/dead-code/stale-terminology audit

## Newly confirmed debt during audit

- A standalone value-returning call compiles through host-C but can produce an unused generated-C temporary and `-Wunused-variable`; this is now recorded for the backend cleanup.
- Default parameters were never a functioning call feature: both ordinary and method calls reject omitted arguments. Their original implementation was parser/AST-only.
