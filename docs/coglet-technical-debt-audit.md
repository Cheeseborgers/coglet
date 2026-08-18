# Coglet technical-debt audit — 2026-08-18

## Authoritative baseline

Source: `coglet_cpy.tar(20260818-042400).gz`

- HEAD: `13d7d87 cleanup(generics): consolidate specialization infrastructure`
- Uploaded working tree: clean before milestone-5 work began.
- Debug / LLVM OFF baseline before milestone 5: 710/710 passed.
- Release / LLVM OFF baseline before milestone 5: 710/710 passed.
- GCC configure/build warnings: 0.
- Clang 17 configure/build warnings: 0.
- LLVM development configuration is not present in the audit container.

Unlike the older audit baseline, the current uploaded source contains the tracked
root-level smoke artifacts (`mem_temp.cog`, `struct_test.cog`, `struct_test_llvm`,
`test_type_directed_math.cog`, `test_vectors.cog`, and `test_vectors_llvm`). The
current upload is authoritative, so those files are treated as present until the
final docs/test-suite consolidation decides their intended disposition.

The source contains `resource`/`move`, `FixedArena`, `Scratch`, `Pool<T>`,
`copyable`, and `DebugAllocator`. Several earlier feature changes were bundled into
historical commits rather than appearing as separate milestone commits; this is
history hygiene, not missing source functionality.

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

3. **Semantic call-resolution consolidation — completed**
   - centralize exact argument checking and concrete call selection used by ordinary calls, methods, overloads, operators, and generic specializations
   - preserve current diagnostics and exact-match behavior
   - do not add conversion ranking or new overload features

4. **Generic specialization consolidation — completed**
   - unify common specialization-key/cache/recursion/name machinery for generic functions and generic structs
   - audit lazy generic-method checking and synthetic semantic-info handling
   - preserve `SemDeclId + structural concrete arguments` identity

5. **Ownership/resource-flow correctness audit — completed in current working tree**
   - replace the Boolean initialization slot with a three-state possibility lattice: definitely initialized, definitely uninitialized, and maybe initialized
   - preserve ordinary definite-assignment behavior while requiring resource reassignment to be definitely uninitialized
   - make `if`, `switch`, and short-circuit `&&` / `||` ownership merging path-sensitive
   - enforce a resource backedge invariant for `while` / `for`, including `continue` and post-expression paths, without adding a general borrow checker or loop fixed point
   - use the post-condition state for condition-false exits and only reachable `break` exits for compile-time-true loops
   - fix `defer` flow snapshotting so checking a deferred `move` does not invalidate the owner at registration time
   - keep `deinit` an ordinary method name; calling it does not implicitly change compiler ownership state
   - preserve the intentionally non-borrow-checked pointer/slice model

6. **Slice/layout/memory-model normalization — next**
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

## Milestone-5 defects fixed

- A move on only some continuing `if`/`switch` paths previously collapsed to the same state as a definite move, allowing a fresh resource assignment to overwrite a live owner on another runtime path.
- Dynamic short-circuit expressions previously applied RHS ownership effects unconditionally; compile-time-skipped RHS moves could also poison the surrounding flow state.
- Loop checking previously had no resource backedge invariant, so a resource moved in a body/post/continue path could be moved again on a later iteration.
- A loop that could terminate normally ignored ownership effects from a condition evaluation and considered only the pre-loop incoming state.
- Compile-time-true loops with reachable `break` paths invented a zero-iteration incoming path instead of using only actual break exits.
- `defer` used a shallow `FlowState` save, so checking `defer consume(move value)` mutated the live initialization array and made the move appear to happen at registration time.
- The resource overwrite diagnostic/documentation implied `deinit()` cleared compiler ownership state even though ordinary method calls never did so.

## Regression coverage added

A new valid ownership-flow fixture plus targeted invalid fixtures cover:

- both-branch and partial-branch moves
- exhaustive and path-dependent switch ownership
- constant and dynamic short-circuit moves
- while-body, `continue`, and `for` post backedges
- condition-time resource transfer with restoration before repeat
- dynamic condition exit merged with a moved `break` exit
- compile-time-true break-only exit state
- deferred move checked without registration-time invalidation
- explicit confirmation that an ordinary `deinit()` call does not clear ownership state

Existing resource fixtures continue to cover by-value parameter/return transfer,
copy rejection, use-after-move, resource containment, `copyable` exclusion,
pointer-receiver methods, and move-after-defer rejection.

## Validation after milestone 5

- Debug / LLVM OFF: 721/721 passed
- Release / LLVM OFF: 721/721 passed
- Clang 17 Debug / LLVM OFF: 721/721 passed
- GCC configure/build warnings: 0
- Clang 17 configure/build warnings: 0
- `git diff --check`: clean

Milestone 6, slice/layout/memory-model normalization, is the next cleanup boundary.
