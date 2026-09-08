# 23 — PathEval: bounded, path-sensitive resolution

`analysis::ImageEval` (docs/05) answers "what can this expression be,
anywhere in the function" — sound, whole-function, and blind to *which* path
got here. That is enough for the vast majority of obfuscated dispatch: a
`select` over two tables, a value set read straight off a relocated slot.
It is not enough for one specific, recurring shape: an index computed
*differently on each arm* of an earlier branch, spilled to a stack slot on
every arm, and reloaded once at the merge point right before the indirect
jump. `ImageEval` sees the merge's phi (or the reload, once stack-slot
promotion has run) as the *union* of every arm's value — correct as a
whole-function statement, but a table-enumeration or value-set candidate
computed from a top/union index is either `top` or a set too wide to prove
every entry lands on a real block, so the branch stays unresolved even
though each individual path through the function determines the index
outright.

PathEval closes that gap the same way the rest of this pass family works:
bounded, sound, and a fallback — never the first thing tried, never allowed
to invent an edge it cannot prove.

## 1. Where this sits

`passes/resolve_indirect.cpp`'s `resolveOne` tries three candidate sources
in order, each strictly more expensive than the last, each returning early
once one succeeds:

1. **`tableCandidates`** — `analysis::jump_table.h`'s whole-table
   enumeration, when the index is provably finite (docs/05).
2. **`valueSetCandidates`** — `analysis::ImageEval`'s bounded value set for
   the target expression directly, when it is not `top`.
3. **`pathEvalCandidates`** — this document. Only reached when both of the
   above come back empty for *this exact branch*.

```cpp
std::vector<uint64_t> candidates =
    tableCandidates(context, eval, dominators, blockId, operands[0]);
if (candidates.empty()) {
  candidates = valueSetCandidates(eval, operands[0]);
}
if (candidates.empty()) {
  candidates = pathEvalCandidates(pathExplorer, branchVa);
}
```

`PathExplorer` is constructed once per `resolveOne` call (mirroring the
pass's existing "one `Dominators` snapshot for the whole pass" reasoning)
and is lazy: `explore()` walks the function on its first call and memoises
the result, so a function whose branches all resolve from the first two
sources pays nothing for this at all.

## 2. The four new types

| type | role | file |
|------|------|------|
| `SymValueSet` | the bounded value domain (cap 16, degrades to `top`) | `analysis/sym_value.h` |
| `PathState` | one in-flight walk's local knowledge: SSA value bindings, an expression memo, a local store | `analysis/path_state.h` |
| `PathInterpreter` | executes one block's ops against a `PathState`, stopping at its terminator | `analysis/path_interpreter.h` |
| `PathExplorer` | bounded BFS/DFS over the whole function, forking `PathState` at conditional branches | `analysis/path_explorer.h` |

`SymValueSet` is deliberately not `analysis::ValueSet` reused: the two only
look similar. `ValueSet`'s arithmetic answers "what can this expression be
across every path that reaches this point" (a union); `SymValueSet`'s
arithmetic answers "what can this expression be *on the one path this
`PathState` represents*". Sharing the type would blur that distinction at
every call site; a five-method value-domain class is cheap enough to just
have twice, once per question.

### 2.1 What a `PathState` knows that `ImageEval` cannot

- **A phi resolves to the one edge this path took**, not the union of every
  incoming edge. `PathInterpreter::stepBlock` finds which predecessor
  `cameFrom` actually is and binds the phi's *own* value from *that*
  operand alone (`PathState::bindPhi`); an edge it cannot identify (a stray
  `cameFrom`, defensive against a bug elsewhere in this pipeline, not
  something the real block graph produces) binds to `top` rather than
  guessing.
- **A load reads back an earlier store on the same path**, even when the
  address is a stack slot no image byte ever defined. `PathState` keeps a
  `localStore_` map (`recordStore`/`loadFrom`) that a plain image evaluator
  has no equivalent of, because `ImageEval` only ever reads the binary's own
  bytes.
- **A store through an address this path cannot pin down invalidates the
  whole local store**, not just the slots it might alias. `recordStore`
  treats more than four candidate addresses, or a `top` address, as "forget
  everything" — conservative on purpose, since keeping stale entries around
  risks reading a value that shape no longer holds.

## 3. How exploration is bounded

Path explosion is the entire risk this design exists to manage; every knob
below exists because an obfuscated function's CFG can be large enough that
"explore everything" is not a plan, it is a hang.

```cpp
struct PathExploreOptions {
  bool enabled = true;
  bool exploreCanaryFailPaths = false;
  unsigned maxPaths = 64;
  unsigned maxStepsPerPath = 4096;
  unsigned maxBlockRevisits = 2;
};
```

- **`maxPaths`** caps the total number of worklist items popped, across the
  *whole* exploration — not per branch, per block. Once the cap is hit,
  `explore()` simply stops; whatever it already proved about earlier
  branches stands, and it is used, not discarded.
- **`maxBlockRevisits`** stops a loop from being unrolled indefinitely: a
  `PathState` that would visit the same block a third time is dropped
  instead of continuing. Two visits is enough to distinguish "first
  iteration" from "steady state" for the loop shapes this targets
  (index-accumulation dispatch loops), without unrolling a real loop's
  full trip count.
- **`maxStepsPerPath`** is the per-path backstop under the two caps above —
  cheap insurance against a block-visit accounting bug turning into an
  unbounded walk.
- **Conditional branches fork only when they must.** If a condition's
  `SymValueSet` is a singleton, `PathExplorer::explore` pushes only the
  taken edge — one path, not two — which is the common case once a few
  arms have already pinned values down. Only a genuinely undetermined
  condition costs a fork.
- **A resolved indirect branch (by an earlier pass, or by
  `tableCandidates`/`valueSetCandidates` on a *different* branch in the
  same function) is not re-explored**: `PathInterpreter::stepBlock` reads
  `il::OpCode::IndirectBranch`'s existing `targets()` when present, and
  `PathExplorer` forks across those (bounded by the same `maxPaths`)
  instead of re-deriving them.

None of this makes PathEval complete — it is a best-effort bounded search,
not a solver. A branch it cannot resolve after the caps run out is treated
exactly like a branch none of the three sources answered: unresolved,
reported as a discovery if any candidates were found before the caps hit
(there are none, in that case — this is a strict subset of the guarantee
`tableCandidates`/`valueSetCandidates` give), and sealed as an opaque
terminator if `--allow-unresolved` is set (docs/05).

## 4. Stack-canary arms are not explored by default

`-fstack-protector`'s idiom (docs/09 shape N, docs/22 §7) puts a rare-path
branch in front of nearly every function of any size: compare the saved
canary, and on mismatch call `__stack_chk_fail` (or an equivalent) and never
return. Exploring that arm buys nothing — it is dead in any non-corrupted
run — and costs a real fraction of the path budget on functions that have
several such checks (docs/22's target function has two).

`PathExplorer::explore` identifies a canary check's mismatch arm
structurally, the same way the rest of this codebase avoids depending on
export-trie symbol resolution it does not have (docs/22 §7's own reasoning
for `sub_18aaf7d50`): `looksLikeBareCallBlock` recognises a block that is
exactly one `Call` followed by `Unreachable`/`Return` and nothing else.
Given a `StackCanarySave`'s check op (`analysis::findStackCanarySaves`,
docs/09), if exactly one of its two successors matches that shape, that
successor is recorded as the canary-fail arm and is skipped when pushing
the check's `CondBranch` outcome — unless `exploreCanaryFailPaths` is set,
for the rare case where the recovery path itself is the thing under
analysis.

This is a heuristic, not a proof that the arm is unreachable: a function
whose "real" logic happens to end in a single bare call also matches. It
only ever *skips* a path PathEval would otherwise have explored — it never
suppresses a candidate the other two sources found, and it never seals a
branch that would otherwise have resolved. Getting it wrong costs
completeness, not soundness.

## 5. What this does and does not close

Applied to `sub_192464D44` (docs/22), PathEval resolves nothing further:
the one branch that stays sealed at `0x19246500c` is not a single-function,
finite-index shape at all. Docs/22 §7 traces exactly why — the index comes
from `x20`/`x25`, callee-saved registers neither this function nor its
tail-callee ever assign, inherited from *whichever code first entered this
dispatcher*, and closing it needs characterising a whole command-dispatch
surface spanning several functions and their call sites, not a deeper
search within this one function's own CFG. A bigger `maxPaths` would not
help; the fact this branch needs does not exist inside the function being
explored. That is the honest boundary: PathEval is single-function and
forward-only, by design (docs/05's "sound over clever" stance, applied to
exploration the same way it is applied to the evaluator and the rewriter),
and the sealed branch it leaves alone is real, not a bug in the caps.

Where PathEval does help is the shape it was built for:
`tests/passes/test_resolve_indirect_path_eval.cpp`'s fixture — an index
computed on each arm of a branch, spilled to a shared stack slot, reloaded
at the merge — resolves only through `pathEvalCandidates`; disabling
path-explore (`--no-path-explore`) leaves the identical branch unresolved,
which is exactly the regression test for "this fallback is doing real
work, not standing in for something the first two sources already covered."

## 6. CLI

| flag | effect |
|------|--------|
| `--no-path-explore` | Disables `pathEvalCandidates` outright; the first two candidate sources are unaffected. Use to isolate whether a resolution came from PathEval or from `tableCandidates`/`valueSetCandidates`. |
| `--path-explore-max-paths <n>` | Overrides `PathExploreOptions::maxPaths` (default 64). |
| `--path-explore-canary-fail` | Sets `exploreCanaryFailPaths`; also explores the recovery arm of every recognised stack-canary check. |

(`maxStepsPerPath` and `maxBlockRevisits` have no CLI knob yet — no sample
has needed one; `PathExploreOptions` is where they would go.)

## 7. Threading `PathExploreOptions` through

`PathExploreOptions` follows the exact pattern `EntryRegFacts` already
established for keeping `xdec_pass` decoupled from `xdec_analysis`'s
concrete types: `pass::Context`/`pass::Manager` hold a
`const analysis::PathExploreOptions*` behind a forward declaration, never
an owned value.

```
DriverOptions::pathExplore   (decompile/driver.h)
        |
Manager::setPathExploreOptions   (pass/manager.h/.cpp)
        |
Context::setPathExploreOptions   (pass/pass.h, set once per Manager::run)
        |
context.pathExploreOptions()     (read by resolve_indirect.cpp)
```

A `nullptr` (a `Context` built outside a `Manager`, as the pass-level unit
tests do) falls back to `PathExploreOptions`'s own defaults — the same
"absence means default, not disabled" rule `context.entryRegFacts()`
already follows.

## 8. Tests

| file | covers |
|------|--------|
| `tests/analysis/test_sym_value.cpp` | `SymValueSet`: top/empty/one, cap degradation, `unite`'s absorption rule |
| `tests/analysis/test_path_interpreter.cpp` | Cross-block phi selection by actual predecessor, store-to-load forwarding, invalidation on an unpinned store address |
| `tests/analysis/test_path_explorer.cpp` | The two-arm-spill-merge shape end to end, `enabled = false` as a no-op, canary-arm suppression on by default and liftable via `exploreCanaryFailPaths` |
| `tests/passes/test_resolve_indirect_path_eval.cpp` | The same shape through the actual `resolve-indirect` pass, both resolving via PathEval and staying unresolved with `--no-path-explore`'s equivalent option set |
